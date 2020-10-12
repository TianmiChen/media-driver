/*
* Copyright (c) 2019-2020, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     mos_context_specific_next.cpp
//! \brief    Container for Linux/Android specific parameters shared across different GPU contexts of the same device instance 
//!

#include "mos_os.h"
#include "mos_util_debug_next.h"
#include "mos_resource_defs.h"
#include <unistd.h>
#include <dlfcn.h>
#include "hwinfo_linux.h"
#include <stdlib.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <time.h>

#if MOS_MEDIASOLO_SUPPORTED
#include "mos_os_solo.h"
#endif // MOS_MEDIASOLO_SUPPORTED
#include "mos_solo_generic.h"
#include "mos_context_specific_next.h"
#include "mos_gpucontextmgr_next.h"
#include "mos_cmdbufmgr_next.h"
#include "media_user_settings_mgr.h"
#define BATCH_BUFFER_SIZE 0x80000

OsContextSpecificNext::OsContextSpecificNext()
{
    MOS_OS_FUNCTION_ENTER;
}

OsContextSpecificNext::~OsContextSpecificNext()
{
    MOS_OS_FUNCTION_ENTER;
}

MOS_STATUS OsContextSpecificNext::Init(DDI_DEVICE_CONTEXT ddiDriverContext)
{
    uint32_t      iDeviceId = 0;
    MOS_STATUS    eStatus;
    uint32_t      i = 0;

    MOS_OS_FUNCTION_ENTER;

    eStatus = MOS_STATUS_SUCCESS;

    PMOS_CONTEXT osDriverContext = (PMOS_CONTEXT)ddiDriverContext;

    if (GetOsContextValid() == false)
    {
        if( nullptr == osDriverContext  ||
            0 >= osDriverContext->fd )
        {
            MOS_OS_ASSERT(false);
            return MOS_STATUS_INVALID_HANDLE;
        }
        m_fd = osDriverContext->fd;

        m_bufmgr = mos_bufmgr_gem_init(m_fd, BATCH_BUFFER_SIZE);
        if (nullptr == m_bufmgr)
        {
            MOS_OS_ASSERTMESSAGE("Not able to allocate buffer manager, fd=0x%d", m_fd);
            return MOS_STATUS_INVALID_PARAMETER;
        }
        mos_bufmgr_gem_enable_reuse(m_bufmgr);
        osDriverContext->bufmgr                 = m_bufmgr;

        //Latency reducation:replace HWGetDeviceID to get device using ioctl from drm.
        iDeviceId   = mos_bufmgr_gem_get_devid(m_bufmgr);
        m_isAtomSOC = IS_ATOMSOC(iDeviceId);

        m_skuTable.reset();
        m_waTable.reset();
        MosUtilities::MosZeroMemory(&m_platformInfo, sizeof(m_platformInfo));
        MosUtilities::MosZeroMemory(&m_gtSystemInfo, sizeof(m_gtSystemInfo));

        eStatus = NullHW::Init(osDriverContext);
        if (!NullHW::IsEnabled())
        {
            eStatus = HWInfo_GetGfxInfo(m_fd, m_bufmgr, &m_platformInfo, &m_skuTable, &m_waTable, &m_gtSystemInfo);
        }
        else
        {
            m_platformInfo = osDriverContext->platform;
            m_skuTable = osDriverContext->SkuTable;
            m_waTable = osDriverContext->WaTable;
            m_gtSystemInfo = osDriverContext->gtSystemInfo;
            iDeviceId = osDriverContext->iDeviceId;
        }

        if (eStatus != MOS_STATUS_SUCCESS)
        {
            MOS_OS_ASSERTMESSAGE("Fatal error - unsuccesfull Sku/Wa/GtSystemInfo initialization");
            return eStatus;
        }

        if (MEDIA_IS_SKU(&m_skuTable, FtrEnableMediaKernels) == 0)
        {
            MEDIA_WR_WA(&m_waTable, WaHucStreamoutOnlyDisable, 0);
        }

        MediaUserSettingsMgr::MediaUserSettingsInit(m_platformInfo.eProductFamily);

        MosUtilities::MosTraceSetupInfo(
            (VA_MAJOR_VERSION << 16) | VA_MINOR_VERSION,
            m_platformInfo.eProductFamily,
            m_platformInfo.eRenderCoreFamily,
            (m_platformInfo.usRevId << 16) | m_platformInfo.usDeviceID);

        GMM_SKU_FEATURE_TABLE   gmmSkuTable = {};
        GMM_WA_TABLE            gmmWaTable  = {};
        GMM_GT_SYSTEM_INFO      gmmGtInfo   = {};
        eStatus = HWInfo_GetGmmInfo(m_fd, &gmmSkuTable, &gmmWaTable, &gmmGtInfo);
        if (MOS_STATUS_SUCCESS != eStatus)
        {
            MOS_OS_ASSERTMESSAGE("Fatal error - unsuccesfull Gmm Sku/Wa/GtSystemInfo initialization");
            return eStatus;
        }

        GmmExportEntries gmmFuncs  = {};
        GMM_STATUS       gmmStatus = OpenGmm(&gmmFuncs);
        if (gmmStatus != GMM_SUCCESS)
        {
            MOS_OS_ASSERTMESSAGE("Fatal error - gmm init failed.");
            return MOS_STATUS_INVALID_PARAMETER;
        }

        // init GMM context
        gmmStatus = gmmFuncs.pfnCreateSingletonContext(m_platformInfo,
            &gmmSkuTable,
            &gmmWaTable,
            &gmmGtInfo);

        if (gmmStatus != GMM_SUCCESS)
        {
            MOS_OS_ASSERTMESSAGE("Fatal error - gmm CreateSingletonContext failed.");
            return MOS_STATUS_INVALID_PARAMETER;
        }
        m_gmmClientContext = gmmFuncs.pfnCreateClientContext((GMM_CLIENT)GMM_LIBVA_LINUX);

        m_auxTableMgr = AuxTableMgr::CreateAuxTableMgr(m_bufmgr, &m_skuTable);

        MOS_USER_FEATURE_VALUE_DATA UserFeatureData;
        MOS_ZeroMemory(&UserFeatureData, sizeof(UserFeatureData));
#if (_DEBUG || _RELEASE_INTERNAL)
        MOS_UserFeature_ReadValue_ID(
            nullptr,
            __MEDIA_USER_FEATURE_VALUE_SIM_ENABLE_ID,
            &UserFeatureData,
            osDriverContext);
#endif
        osDriverContext->bSimIsActive = (int32_t)UserFeatureData.i32Data;

        m_useSwSwizzling = osDriverContext->bSimIsActive || MEDIA_IS_SKU(&m_skuTable, FtrUseSwSwizzling);

        m_tileYFlag      = MEDIA_IS_SKU(&m_skuTable, FtrTileY);

        m_use64BitRelocs = true;

        if (!NullHW::IsEnabled())
        {
            osDriverContext->iDeviceId              = iDeviceId;
            osDriverContext->SkuTable               = m_skuTable;
            osDriverContext->WaTable                = m_waTable;
            osDriverContext->gtSystemInfo           = m_gtSystemInfo;
            osDriverContext->platform               = m_platformInfo;
        }
        osDriverContext->pGmmClientContext      = m_gmmClientContext;
        osDriverContext->m_auxTableMgr          = m_auxTableMgr;
        osDriverContext->bUseSwSwizzling        = m_useSwSwizzling;
        osDriverContext->bTileYFlag             = m_tileYFlag;
        osDriverContext->bIsAtomSOC             = m_isAtomSOC;
        osDriverContext->m_osDeviceContext      = this;

        m_usesPatchList             = true;
        m_usesGfxAddress            = false;

        SetOsContextValid(true);
        // Prepare the command buffer manager
        m_cmdBufMgr = CmdBufMgrNext::GetObject();
        MOS_OS_CHK_NULL_RETURN(m_cmdBufMgr);
        MOS_OS_CHK_STATUS_RETURN(m_cmdBufMgr->Initialize(this, COMMAND_BUFFER_SIZE/2));

        // Prepare the gpu Context manager
        m_gpuContextMgr = GpuContextMgrNext::GetObject(&m_gtSystemInfo, this);
        MOS_OS_CHK_NULL_RETURN(m_gpuContextMgr);

        //It must be done with m_gpuContextMgr ready. Insides it will create gpu context.
#ifdef _MMC_SUPPORTED
        m_mosDecompression = MOS_New(MosDecompression, osDriverContext);
        MOS_OS_CHK_NULL_RETURN(m_mosDecompression);
        osDriverContext->ppMediaMemDecompState = m_mosDecompression->GetMediaMemDecompState();
        MOS_OS_CHK_NULL_RETURN(osDriverContext->ppMediaMemDecompState);
        if (*osDriverContext->ppMediaMemDecompState == nullptr)
        {
            MOS_OS_NORMALMESSAGE("Decomp state creation failed");
        }
#endif
        m_mosMediaCopy = MOS_New(MosMediaCopy, osDriverContext);
        MOS_OS_CHK_NULL_RETURN(m_mosMediaCopy);
        osDriverContext->ppMediaCopyState = m_mosMediaCopy->GetMediaCopyState();
        MOS_OS_CHK_NULL_RETURN(osDriverContext->ppMediaCopyState);
        if (*osDriverContext->ppMediaCopyState == nullptr)
        {
            MOS_OS_ASSERTMESSAGE("Media Copy state creation failed");
        }
    }
    return eStatus;
}

void OsContextSpecificNext::Destroy()
{
    MOS_OS_FUNCTION_ENTER;

    if (GetOsContextValid() == true)
    {
        if (m_auxTableMgr != nullptr)
        {
            MOS_Delete(m_auxTableMgr);
            m_auxTableMgr = nullptr;
        }

        m_skuTable.reset();
        m_waTable.reset();

        mos_bufmgr_destroy(m_bufmgr);

        GmmExportEntries GmmFuncs;
        GMM_STATUS       gmmStatus = OpenGmm(&GmmFuncs);
        if (gmmStatus == GMM_SUCCESS)
        {
            GmmFuncs.pfnDeleteClientContext((GMM_CLIENT_CONTEXT *)m_gmmClientContext);
            m_gmmClientContext = nullptr;
            GmmFuncs.pfnDestroySingletonContext();
        }
        else
        {
            MOS_OS_ASSERTMESSAGE("gmm init failed.");
        }

        SetOsContextValid(false);
    }
}

