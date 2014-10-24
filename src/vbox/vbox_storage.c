/*
 * Copyright (C) 2014 Taowei Luo (uaedante@gmail.com)
 * Copyright (C) 2010-2014 Red Hat, Inc.
 * Copyright (C) 2008-2009 Sun Microsystems, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "internal.h"
#include "datatypes.h"
#include "domain_conf.h"
#include "domain_event.h"
#include "virlog.h"
#include "virstring.h"

#include "vbox_common.h"
#include "vbox_uniformed_api.h"

#define VIR_FROM_THIS VIR_FROM_VBOX

VIR_LOG_INIT("vbox.vbox_storage");

static vboxUniformedAPI gVBoxAPI;

/**
 * The Storage Functions here on
 */

virDrvOpenStatus vboxStorageOpen(virConnectPtr conn,
                                 virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                 unsigned int flags)
{
    vboxGlobalData *data = conn->privateData;

    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (STRNEQ(conn->driver->name, "VBOX"))
        return VIR_DRV_OPEN_DECLINED;

    if ((!data->pFuncs) || (!data->vboxObj) || (!data->vboxSession))
        return VIR_DRV_OPEN_ERROR;

    VIR_DEBUG("vbox storage initialized");
    /* conn->storagePrivateData = some storage specific data */
    return VIR_DRV_OPEN_SUCCESS;
}

int vboxStorageClose(virConnectPtr conn)
{
    VIR_DEBUG("vbox storage uninitialized");
    conn->storagePrivateData = NULL;
    return 0;
}

int vboxConnectNumOfStoragePools(virConnectPtr conn ATTRIBUTE_UNUSED)
{

    /** Currently only one pool supported, the default one
     * given by ISystemProperties::defaultHardDiskFolder()
     */

    return 1;
}

int vboxConnectListStoragePools(virConnectPtr conn ATTRIBUTE_UNUSED,
                                char **const names, int nnames)
{
    int numActive = 0;

    if (nnames > 0 &&
        VIR_STRDUP(names[numActive], "default-pool") > 0)
        numActive++;
    return numActive;
}

virStoragePoolPtr vboxStoragePoolLookupByName(virConnectPtr conn, const char *name)
{
    virStoragePoolPtr ret = NULL;

    /** Current limitation of the function: since
     * the default pool doesn't have UUID just assign
     * one till vbox can handle pools
     */
    if (STREQ("default-pool", name)) {
        unsigned char uuid[VIR_UUID_BUFLEN];
        const char *uuidstr = "1deff1ff-1481-464f-967f-a50fe8936cc4";

        ignore_value(virUUIDParse(uuidstr, uuid));

        ret = virGetStoragePool(conn, name, uuid, NULL, NULL);
    }

    return ret;
}

int vboxStoragePoolNumOfVolumes(virStoragePoolPtr pool)
{
    vboxGlobalData *data = pool->conn->privateData;
    vboxArray hardDisks = VBOX_ARRAY_INITIALIZER;
    PRUint32 hardDiskAccessible = 0;
    nsresult rc;
    size_t i;
    int ret = -1;

    if (!data->vboxObj) {
        return ret;
    }

    rc = gVBoxAPI.UArray.vboxArrayGet(&hardDisks, data->vboxObj,
                                      gVBoxAPI.UArray.handleGetHardDisks(data->vboxObj));
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not get number of volumes in the pool: %s, rc=%08x"),
                       pool->name, (unsigned)rc);
        return ret;
    }

    for (i = 0; i < hardDisks.count; ++i) {
        IHardDisk *hardDisk = hardDisks.items[i];
        PRUint32 hddstate;

        if (!hardDisk)
            continue;

        gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
        if (hddstate != MediaState_Inaccessible)
            hardDiskAccessible++;
    }

    gVBoxAPI.UArray.vboxArrayRelease(&hardDisks);

    ret = hardDiskAccessible;

    return ret;
}

int vboxStoragePoolListVolumes(virStoragePoolPtr pool, char **const names, int nnames)
{
    vboxGlobalData *data = pool->conn->privateData;
    vboxArray hardDisks = VBOX_ARRAY_INITIALIZER;
    PRUint32 numActive = 0;
    nsresult rc;
    size_t i;
    int ret = -1;

    if (!data->vboxObj) {
        return ret;
    }

    rc = gVBoxAPI.UArray.vboxArrayGet(&hardDisks, data->vboxObj,
                                      gVBoxAPI.UArray.handleGetHardDisks(data->vboxObj));
    if (NS_FAILED(rc)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("could not get the volume list in the pool: %s, rc=%08x"),
                       pool->name, (unsigned)rc);
        return ret;
    }

    for (i = 0; i < hardDisks.count && numActive < nnames; ++i) {
        IHardDisk *hardDisk = hardDisks.items[i];
        PRUint32 hddstate;
        char *nameUtf8 = NULL;
        PRUnichar *nameUtf16 = NULL;

        if (!hardDisk)
            continue;

        gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
        if (hddstate == MediaState_Inaccessible)
            continue;

        gVBoxAPI.UIMedium.GetName(hardDisk, &nameUtf16);

        VBOX_UTF16_TO_UTF8(nameUtf16, &nameUtf8);
        VBOX_UTF16_FREE(nameUtf16);

        if (!nameUtf8)
            continue;

        VIR_DEBUG("nnames[%d]: %s", numActive, nameUtf8);
        if (VIR_STRDUP(names[numActive], nameUtf8) > 0)
            numActive++;

        VBOX_UTF8_FREE(nameUtf8);
    }

    gVBoxAPI.UArray.vboxArrayRelease(&hardDisks);
    ret = numActive;

    return ret;
}

virStorageVolPtr vboxStorageVolLookupByName(virStoragePoolPtr pool, const char *name)
{
    vboxGlobalData *data = pool->conn->privateData;
    vboxArray hardDisks = VBOX_ARRAY_INITIALIZER;
    nsresult rc;
    size_t i;
    virStorageVolPtr ret = NULL;

    if (!data->vboxObj) {
        return ret;
    }

    if (!name)
        return ret;

    rc = gVBoxAPI.UArray.vboxArrayGet(&hardDisks, data->vboxObj,
                                      gVBoxAPI.UArray.handleGetHardDisks(data->vboxObj));
    if (NS_FAILED(rc))
        return ret;

    for (i = 0; i < hardDisks.count; ++i) {
        IHardDisk *hardDisk = hardDisks.items[i];
        PRUint32 hddstate;
        char *nameUtf8 = NULL;
        PRUnichar *nameUtf16 = NULL;

        if (!hardDisk)
            continue;

        gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
        if (hddstate == MediaState_Inaccessible)
            continue;

        gVBoxAPI.UIMedium.GetName(hardDisk, &nameUtf16);

        if (nameUtf16) {
            VBOX_UTF16_TO_UTF8(nameUtf16, &nameUtf8);
            VBOX_UTF16_FREE(nameUtf16);
        }

        if (nameUtf8 && STREQ(nameUtf8, name)) {
            vboxIIDUnion hddIID;
            unsigned char uuid[VIR_UUID_BUFLEN];
            char key[VIR_UUID_STRING_BUFLEN] = "";

            VBOX_IID_INITIALIZE(&hddIID);
            rc = gVBoxAPI.UIMedium.GetId(hardDisk, &hddIID);
            if (NS_SUCCEEDED(rc)) {
                vboxIIDToUUID(&hddIID, uuid);
                virUUIDFormat(uuid, key);

                ret = virGetStorageVol(pool->conn, pool->name, name, key,
                                       NULL, NULL);

                VIR_DEBUG("virStorageVolPtr: %p", ret);
                VIR_DEBUG("Storage Volume Name: %s", name);
                VIR_DEBUG("Storage Volume key : %s", key);
                VIR_DEBUG("Storage Volume Pool: %s", pool->name);
            }

            vboxIIDUnalloc(&hddIID);
            VBOX_UTF8_FREE(nameUtf8);
            break;
        }

        VBOX_UTF8_FREE(nameUtf8);
    }

    gVBoxAPI.UArray.vboxArrayRelease(&hardDisks);

    return ret;
}

virStorageVolPtr vboxStorageVolLookupByKey(virConnectPtr conn, const char *key)
{
    vboxGlobalData *data = conn->privateData;
    vboxIIDUnion hddIID;
    unsigned char uuid[VIR_UUID_BUFLEN];
    IHardDisk *hardDisk = NULL;
    PRUnichar *hddNameUtf16 = NULL;
    char *hddNameUtf8 = NULL;
    PRUint32 hddstate;
    nsresult rc;
    virStorageVolPtr ret = NULL;

    if (!data->vboxObj) {
        return ret;
    }

    VBOX_IID_INITIALIZE(&hddIID);
    if (!key)
        return ret;

    if (virUUIDParse(key, uuid) < 0) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("Could not parse UUID from '%s'"), key);
        return NULL;
    }

    vboxIIDFromUUID(&hddIID, uuid);
    rc = gVBoxAPI.UIVirtualBox.GetHardDiskByIID(data->vboxObj, &hddIID, &hardDisk);
    if (NS_FAILED(rc))
        goto cleanup;

    gVBoxAPI.UIMedium.GetState(hardDisk, &hddstate);
    if (hddstate == MediaState_Inaccessible)
        goto cleanup;

    gVBoxAPI.UIMedium.GetName(hardDisk, &hddNameUtf16);
    if (!hddNameUtf16)
        goto cleanup;

    VBOX_UTF16_TO_UTF8(hddNameUtf16, &hddNameUtf8);
    if (!hddNameUtf8) {
        VBOX_UTF16_FREE(hddNameUtf16);
        goto cleanup;
    }

    if (vboxConnectNumOfStoragePools(conn) == 1) {
        ret = virGetStorageVol(conn, "default-pool", hddNameUtf8, key,
                               NULL, NULL);
        VIR_DEBUG("Storage Volume Pool: %s", "default-pool");
    } else {
        /* TODO: currently only one default pool and thus
         * nothing here, change it when pools are supported
         */
    }

    VIR_DEBUG("Storage Volume Name: %s", key);
    VIR_DEBUG("Storage Volume key : %s", hddNameUtf8);

    VBOX_UTF8_FREE(hddNameUtf8);
    VBOX_UTF16_FREE(hddNameUtf16);

 cleanup:
    VBOX_MEDIUM_RELEASE(hardDisk);
    vboxIIDUnalloc(&hddIID);
    return ret;
}