/* $Id$ */
/** @file
 * HM SVM (AMD-V) - All contexts.
 */

/*
 * Copyright (C) 2017 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_HM
#include "HMInternal.h"
#include <VBox/vmm/apic.h>
#include <VBox/vmm/gim.h>
#include <VBox/vmm/hm.h>
#include <VBox/vmm/vm.h>
#include <VBox/vmm/hm_svm.h>


#ifndef IN_RC
/**
 * Emulates a simple MOV TPR (CR8) instruction, used for TPR patching on 32-bit
 * guests. This simply looks up the patch record at EIP and does the required.
 *
 * This VMMCALL is used a fallback mechanism when mov to/from cr8 isn't exactly
 * like how we want it to be (e.g. not followed by shr 4 as is usually done for
 * TPR). See hmR3ReplaceTprInstr() for the details.
 *
 * @returns VBox status code.
 * @retval VINF_SUCCESS if the access was handled successfully.
 * @retval VERR_NOT_FOUND if no patch record for this RIP could be found.
 * @retval VERR_SVM_UNEXPECTED_PATCH_TYPE if the found patch type is invalid.
 *
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pCtx                Pointer to the guest-CPU context.
 * @param   pfUpdateRipAndRF    Whether the guest RIP/EIP has been updated as
 *                              part of the TPR patch operation.
 */
static int hmSvmEmulateMovTpr(PVMCPU pVCpu, PCPUMCTX pCtx, bool *pfUpdateRipAndRF)
{
    Log4(("Emulated VMMCall TPR access replacement at RIP=%RGv\n", pCtx->rip));

    /*
     * We do this in a loop as we increment the RIP after a successful emulation
     * and the new RIP may be a patched instruction which needs emulation as well.
     */
    bool fUpdateRipAndRF = false;
    bool fPatchFound     = false;
    PVM  pVM = pVCpu->CTX_SUFF(pVM);
    for (;;)
    {
        bool    fPending;
        uint8_t u8Tpr;

        PHMTPRPATCH pPatch = (PHMTPRPATCH)RTAvloU32Get(&pVM->hm.s.PatchTree, (AVLOU32KEY)pCtx->eip);
        if (!pPatch)
            break;

        fPatchFound = true;
        switch (pPatch->enmType)
        {
            case HMTPRINSTR_READ:
            {
                int rc = APICGetTpr(pVCpu, &u8Tpr, &fPending, NULL /* pu8PendingIrq */);
                AssertRC(rc);

                rc = DISWriteReg32(CPUMCTX2CORE(pCtx), pPatch->uDstOperand, u8Tpr);
                AssertRC(rc);
                pCtx->rip += pPatch->cbOp;
                pCtx->eflags.Bits.u1RF = 0;
                fUpdateRipAndRF = true;
                break;
            }

            case HMTPRINSTR_WRITE_REG:
            case HMTPRINSTR_WRITE_IMM:
            {
                if (pPatch->enmType == HMTPRINSTR_WRITE_REG)
                {
                    uint32_t u32Val;
                    int rc = DISFetchReg32(CPUMCTX2CORE(pCtx), pPatch->uSrcOperand, &u32Val);
                    AssertRC(rc);
                    u8Tpr = u32Val;
                }
                else
                    u8Tpr = (uint8_t)pPatch->uSrcOperand;

                int rc2 = APICSetTpr(pVCpu, u8Tpr);
                AssertRC(rc2);
                HMCPU_CF_SET(pVCpu, HM_CHANGED_SVM_GUEST_APIC_STATE);

                pCtx->rip += pPatch->cbOp;
                pCtx->eflags.Bits.u1RF = 0;
                fUpdateRipAndRF = true;
                break;
            }

            default:
            {
                AssertMsgFailed(("Unexpected patch type %d\n", pPatch->enmType));
                pVCpu->hm.s.u32HMError = pPatch->enmType;
                *pfUpdateRipAndRF = fUpdateRipAndRF;
                return VERR_SVM_UNEXPECTED_PATCH_TYPE;
            }
        }
    }

    *pfUpdateRipAndRF = fUpdateRipAndRF;
    if (fPatchFound)
        return VINF_SUCCESS;
    return VERR_NOT_FOUND;
}
#endif /* !IN_RC */


/**
 * Performs the operations necessary that are part of the vmmcall instruction
 * execution in the guest.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @retval  VINF_SUCCESS on successful handling, no \#UD needs to be thrown,
 *          update RIP and eflags.RF depending on @a pfUpdatedRipAndRF and
 *          continue guest execution.
 * @retval  VINF_GIM_HYPERCALL_CONTINUING continue hypercall without updating
 *          RIP.
 * @retval  VINF_GIM_R3_HYPERCALL re-start the hypercall from ring-3.
 *
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pCtx                Pointer to the guest-CPU context.
 * @param   pfUpdatedRipAndRF   Whether the guest RIP/EIP has been updated as
 *                              part of handling the VMMCALL operation.
 */
VMM_INT_DECL(VBOXSTRICTRC) HMSvmVmmcall(PVMCPU pVCpu, PCPUMCTX pCtx, bool *pfUpdatedRipAndRF)
{
#ifndef IN_RC
    /*
     * TPR patched instruction emulation for 32-bit guests.
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    if (pVM->hm.s.fTprPatchingAllowed)
    {
        int rc = hmSvmEmulateMovTpr(pVCpu, pCtx, pfUpdatedRipAndRF);
        if (RT_SUCCESS(rc))
            return VINF_SUCCESS;

        if (rc != VERR_NOT_FOUND)
        {
            Log(("hmSvmExitVmmCall: hmSvmEmulateMovTpr returns %Rrc\n", rc));
            return rc;
        }
    }
#endif

    /*
     * Paravirtualized hypercalls.
     */
    *pfUpdatedRipAndRF = false;
    if (pVCpu->hm.s.fHypercallsEnabled)
        return GIMHypercall(pVCpu, pCtx);

    return VERR_NOT_AVAILABLE;
}


/**
 * Performs the operations necessary that are part of the vmrun instruction
 * execution in the guest.
 *
 * @returns Strict VBox status code (i.e. informational status codes too).
 * @param   pVCpu               The cross context virtual CPU structure.
 * @param   pCtx                Pointer to the guest-CPU context.
 * @param   GCPhysVmcb          Guest physical address of the VMCB to run.
 */
VMM_INT_DECL(VBOXSTRICTRC) HMSvmVmrun(PVMCPU pVCpu, PCPUMCTX pCtx, RTGCPHYS GCPhysVmcb)
{
    Assert(pVCpu);
    Assert(pCtx);

    /*
     * Cache the physical address of the VMCB for #VMEXIT exceptions.
     */
    pCtx->hwvirt.svm.GCPhysVmcb = GCPhysVmcb;

    /*
     * Cache the VMCB controls.
     */
    PVM pVM = pVCpu->CTX_SUFF(pVM);
    int rc = PGMPhysSimpleReadGCPhys(pVM, &pCtx->hwvirt.svm.VmcbCtrl, GCPhysVmcb, sizeof(pCtx->hwvirt.svm.VmcbCtrl));
    if (RT_SUCCESS(rc))
    {
        /*
         * Save host state.
         */
        PSVMHOSTSTATE pHostState = &pCtx->hwvirt.svm.HostState;
        pHostState->es       = pCtx->es;
        pHostState->cs       = pCtx->cs;
        pHostState->ss       = pCtx->ss;
        pHostState->ds       = pCtx->ds;
        pHostState->gdtr     = pCtx->gdtr;
        pHostState->idtr     = pCtx->idtr;
        pHostState->uEferMsr = pCtx->msrEFER;
        pHostState->uCr0     = pCtx->cr0;
        pHostState->uCr3     = pCtx->cr3;
        pHostState->uCr4     = pCtx->cr4;
        pHostState->rflags   = pCtx->rflags;
        pHostState->uRip     = pCtx->rip;
        pHostState->uRsp     = pCtx->rsp;
        pHostState->uRax     = pCtx->rax;

        /*
         * Validate the VMCB controls.
         */
        if (!CPUMIsGuestSvmCtrlInterceptSet(pCtx, SVM_CTRL_INTERCEPT_VMRUN))
        {
            Log(("HMSvmVmRun: VMRUN instruction not intercepted -> #VMEXIT\n"));
            return HMSvmNstGstVmExit(pVCpu, pCtx, SVM_EXIT_INVALID, 0 /* uExitInfo1 */, 0 /* uExitInfo2 */);
        }
        if (    pCtx->hwvirt.svm.VmcbCtrl.NestedPaging.n.u1NestedPaging
            && !pVM->cpum.ro.GuestFeatures.svm.feat.n.fNestedPaging)
        {
            Log(("HMSvmVmRun: Nested paging not supported -> #VMEXIT\n"));
            return HMSvmNstGstVmExit(pVCpu, pCtx, SVM_EXIT_INVALID, 0 /* uExitInfo1 */, 0 /* uExitInfo2 */);
        }
        if (!pCtx->hwvirt.svm.VmcbCtrl.TLBCtrl.n.u32ASID)
        {
            Log(("HMSvmVmRun: Guest ASID is invalid -> #VMEXIT\n"));
            return HMSvmNstGstVmExit(pVCpu, pCtx, SVM_EXIT_INVALID, 0 /* uExitInfo1 */, 0 /* uExitInfo2 */);
        }

        /** @todo the rest. */

        return VERR_NOT_IMPLEMENTED;
    }

    return rc;
}


/**
 * SVM nested-guest VMEXIT handler.
 *
 * @returns Strict VBox status code.
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pCtx        The guest-CPU context.
 * @param   uExitCode   The exit code.
 * @param   uExitInfo1  The exit info. 1 field.
 * @param   uExitInfo2  The exit info. 2 field.
 */
VMM_INT_DECL(VBOXSTRICTRC) HMSvmNstGstVmExit(PVMCPU pVCpu, PCPUMCTX pCtx, uint64_t uExitCode, uint64_t uExitInfo1,
                                             uint64_t uExitInfo2)
{
    if (   CPUMIsGuestInNestedHwVirtMode(pCtx)
        || uExitCode == SVM_EXIT_INVALID)
    {
        RT_NOREF(pVCpu);

        pCtx->hwvirt.svm.fGif = 0;

        /** @todo implement \#VMEXIT. */

        return VINF_SUCCESS;
    }
    else
    {
        Log(("HMNstGstSvmVmExit: Not in SVM guest mode! uExitCode=%#RX64 uExitInfo1=%#RX64 uExitInfo2=%#RX64\n", uExitCode,
             uExitInfo1, uExitInfo2));
        RT_NOREF2(uExitInfo1, uExitInfo2);
    }

    return VERR_SVM_IPE_5;
}

