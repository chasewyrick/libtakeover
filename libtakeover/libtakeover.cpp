//
//  libtakeover.cpp
//  libtakeover
//
//  Created by tihmstar on 24.02.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//

#define LOCAL_FILENAME "libtakeover.cpp"

#include "all_libtakeover.h"
#include "libtakeover.hpp"
#include "TKexception.hpp"
#include <stdlib.h>
#include <dlfcn.h>

#include <unistd.h>

using namespace tihmstar;

extern "C"{
kern_return_t mach_vm_allocate(vm_map_t target, mach_vm_address_t *address, mach_vm_size_t size, int flags);
kern_return_t mach_vm_protect(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, boolean_t set_maximum, vm_prot_t new_protection);
kern_return_t mach_vm_read_overwrite(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *outsize);
kern_return_t mach_vm_write(vm_map_t target_task, mach_vm_address_t address, vm_offset_t data, mach_msg_type_number_t dataCnt);
kern_return_t mach_vm_deallocate(mach_port_name_t target, mach_vm_address_t address, mach_vm_size_t size);
};

#pragma pack(4)
typedef struct {
    mach_msg_header_t Head;
    mach_msg_body_t msgh_body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t NDR;
} exception_raise_request; // the bits we need at least

typedef struct {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
} exception_raise_reply;
#pragma pack()

takeover::takeover(mach_port_t target):_target(target){
    /* init member vars */
    _remoteStack = NULL;
    _marionetteThread = MACH_PORT_NULL;
    _exceptionHandler = MACH_PORT_NULL;
    _emsg = {};
    
    /* setup local variables */
    uint64_t *localStack = NULL;
    size_t stackpointer = 0;
    arm_thread_state64_t state = {0};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    
    /* setup cleanups for normal/emergency case */
    auto finalClean =[&]{ //always clean up
        safeFree(localStack);
    };
    
    auto clean =[&]{ //cleanup only if something goes wrong
        
        auto err = deinit();
        if(err.first){
            error("[takeover] deinit failed on line %d with code %d",err.first,err.second);
        }
        
        finalClean(); //also do regular cleanup
    };
    
    /* actually construct object */
    
    //aquire send right to targer
    {
        kern_return_t err = 0;
        doassure(!(err = mach_port_insert_right(mach_task_self(),_target, _target, MACH_MSG_TYPE_COPY_SEND)), [&](void)->void{
            //if this step fails, make sure not to drop a send right to the target on cleanup!
            _target = MACH_PORT_NULL;
            assureMachclean(err);
        });

    }

    //allocate remote stack
    assureMachclean(mach_vm_allocate(_target, &_remoteStack, _remoteStackSize, VM_FLAGS_ANYWHERE));
    assureMachclean(mach_vm_protect(_target, _remoteStack, _remoteStackSize, 1, VM_PROT_READ | VM_PROT_WRITE));


    //setup stack
    assureclean(localStack = (uint64_t *)malloc(_remoteStackSize));
    stackpointer = (_remoteStackSize / 8)-1;
    localStack[stackpointer--] = 0x4142434445464748; //magic end (x86 legacy, we don't need this for ARM64, do we?)

    
    //spawn new thread
    assureMachclean(thread_create(_target, &_marionetteThread));

    localStack[0xf8/8] = (uint64_t)_marionetteThread;   //thread port
    localStack[0xe0/8] = (uint64_t)_remoteStack;        //ptr to remotePthreadBuf (lies at the beginning of _remoteStack)
    localStack[0x88/8] = (uint64_t)0x5152535455565758;  //pc (do a soft crash)
    localStack[0x98/8] = 0x1337;                        //pthread arg1
    
    //write localStack to remote stack
    assureMachclean(mach_vm_write(_target, _remoteStack, (vm_offset_t)localStack, (mach_msg_type_number_t)_remoteStackSize));
    
    //setup thread state
    assureMachclean(thread_get_state(_marionetteThread, ARM_THREAD_STATE64, (thread_state_t)&state, &count));
    
    state.__x[0] = (uint64_t)_remoteStack;
    state.__lr = 0x7171717171717171;        //actual magic end
    assureclean(state.__pc = (uint64_t)dlsym(RTLD_NEXT, "thread_start"));
    state.__sp = (uint64_t)(_remoteStack + stackpointer*sizeof(uint64_t));
    
    assureMachclean(thread_set_state(_marionetteThread, ARM_THREAD_STATE64, (thread_state_t)&state, ARM_THREAD_STATE64_COUNT));

    //create exception port
    assureMachclean(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &_exceptionHandler));
    assureMachclean(mach_port_insert_right(mach_task_self(),_exceptionHandler, _exceptionHandler, MACH_MSG_TYPE_MAKE_SEND));
    
    //set our new port
    assureMachclean(thread_set_exception_ports(_marionetteThread, EXC_MASK_ALL & ~(EXC_MASK_MACH_SYSCALL | EXC_MASK_SYSCALL | EXC_MASK_RPC_ALERT), _exceptionHandler, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64));

    //initialize our remote thread
    assureMachclean(thread_resume(_marionetteThread));
    
    
    //wait for exception

    assureMachclean(mach_msg(&_emsg.head, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(_emsg), _exceptionHandler, 0, MACH_PORT_NULL));
    
    finalClean();
}

uint64_t takeover::callfunc(void *addr, const std::vector<uint64_t> &x){
    arm_thread_state64_t state = {0};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;

    retassure(x.size() <= 29,"only up to 29 arguments allowed");
    
    assureMach(thread_get_state(_marionetteThread, ARM_THREAD_STATE64, (thread_state_t)&state, &count));
    
    state.__lr = 0x7171717171717171;
    state.__pc = (uint64_t)addr;
    
    for (int i=0; i<29; i++) {
        state.__x[i] = (i<x.size()) ? x[i] : 0;
    }
    
    assureMach(thread_set_state(_marionetteThread, ARM_THREAD_STATE64, (thread_state_t)&state, ARM_THREAD_STATE64_COUNT));
    
    exception_raise_reply reply = {0};
    exception_raise_request* req = (exception_raise_request*)&_emsg;
    reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req->Head.msgh_bits), 0);
    reply.Head.msgh_size = sizeof(reply);
    reply.Head.msgh_remote_port = req->Head.msgh_remote_port;
    reply.Head.msgh_local_port = MACH_PORT_NULL;
    reply.Head.msgh_id = req->Head.msgh_id + 100;
    
    reply.NDR = req->NDR;
    reply.RetCode = KERN_SUCCESS;
    
    //resume
    assureMach(mach_msg(&reply.Head, MACH_SEND_MSG|MACH_MSG_OPTION_NONE, (mach_msg_size_t)sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL));
    
    //wait until end of function
    assureMach(mach_msg(&_emsg.head, MACH_RCV_MSG|MACH_RCV_LARGE, 0, sizeof(_emsg), _exceptionHandler, 0, MACH_PORT_NULL));
    
    //get result
    assureMach(thread_get_state(_marionetteThread, ARM_THREAD_STATE64, (thread_state_t)&state, &count));
    
    return state.__x[0];
}

void takeover::readMem(void *remote, size_t size, void *outAddr){
    mach_vm_size_t out = size;
    assureMach(mach_vm_read_overwrite(_target, (mach_vm_address_t)remote , (mach_vm_size_t)size, (mach_vm_address_t) outAddr, &out));
}
void takeover::writeMem(void *remote, size_t size, void *inAddr){
    assureMach(mach_vm_write(_target, (mach_vm_address_t)remote, (vm_offset_t)inAddr, (mach_msg_type_number_t)size));
}
void *takeover::allocMem(size_t size){
    void *ret = 0;
    assureMach(mach_vm_allocate(_target, (mach_vm_address_t*)&ret, size, VM_FLAGS_ANYWHERE));
    assureMach(mach_vm_protect(_target, (mach_vm_address_t)ret, size, 1, VM_PROT_READ | VM_PROT_WRITE));
    return ret;
}
void takeover::deallocMem(void *remote,size_t size){
    assureMach(mach_vm_deallocate(_target, (mach_vm_address_t)remote, (mach_vm_size_t)size));
}


std::pair<int, kern_return_t> takeover::deinit(){
    kern_return_t err = 0;
    std::pair<int, kern_return_t> gerr = {0,0};
    if (_marionetteThread) {
        err = thread_terminate(_marionetteThread);
        _marionetteThread = MACH_PORT_NULL;
        if (err) {
            error("deinit: line=%d err=%llx s=%s",__LINE__,(uint64_t)err,mach_error_string(err));
            if (!gerr.first) {gerr.first=__LINE__; gerr.second = err;}
        }
    }
    
    if (_remoteStack) {
        err = mach_vm_deallocate(_target, _remoteStack, _remoteStackSize);
        _remoteStack = NULL;
        if (err) {
            error("deinit: line=%d err=%llx s=%s",__LINE__,(uint64_t)err,mach_error_string(err));
            if (!gerr.first) {gerr.first=__LINE__; gerr.second = err;}
        }
    }
    
    if (_exceptionHandler) {
        err = mach_port_destroy(mach_task_self(), _exceptionHandler);
        _exceptionHandler = MACH_PORT_NULL;
        if (err) {
            error("deinit: line=%d err=%llx s=%s",__LINE__,(uint64_t)err,mach_error_string(err));
            if (!gerr.first) {gerr.first=__LINE__; gerr.second = err;}
        }
    }
    
    if (_target) {
        //drop one send right
        err = mach_port_deallocate(mach_task_self(), _target);
        _target = MACH_PORT_NULL;
        if (err) {
            error("deinit: line=%d err=%llx s=%s",__LINE__,(uint64_t)err,mach_error_string(err));
            if (!gerr.first) {gerr.first=__LINE__; gerr.second = err;}
        }
    }
    
    return gerr;
}


takeover::~takeover(){
    auto err = deinit();
    if(err.first){
        error("[~takeover] deinit failed on line %d with code %d",err.first,err.second);
    }
}
