#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include <string.h>

#include "usb_msc.hpp"
#include "diskio_rawmsc.hpp"
// #define MAX(x,y)                        (((x) >= (y)) ? (x) : (y))

// TODO - make a macro
static void mock_msc_scsi_init_cbw(msc_bulk_cbw_t *cbw, bool is_read, int lun, uint32_t tag, uint8_t *cmd, size_t len)
{
    cbw->dCBWSignature = 0x43425355;            //Fixed value
    cbw->dCBWTag = tag;                         //Random value that is echoed back
    cbw->dCBWDataTransferLength = len;          //num_sectors * block_size;
    cbw->bmCBWFlags = (is_read) ? (1 << 7) : 0; //If this is a read, set the direction flag
    cbw->bCBWLUN = lun;
    cbw->bCBWCBLength = 10; //The length of the SCSI command
    memcpy((void *)&cbw->CBWCB, cmd, 10);
}

void IRAM_ATTR usb_transfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGW("STATUS", "status: %d, bytes: %d", transfer->status, transfer->actual_num_bytes);
    ESP_LOG_BUFFER_HEX("HEX_DATA", transfer->data_buffer, transfer->actual_num_bytes);

    USBmscDevice *dev = (USBmscDevice *)transfer->context;

    if (dev && transfer->actual_num_bytes == 9 && *(uint16_t *)transfer->data_buffer == 0xfea1) // max luns
    {
        dev->onCSW(transfer);
    }
    else if (dev && transfer->actual_num_bytes == 31 && *(uint32_t *)transfer->data_buffer == 0x43425355) // CBW
    {
        dev->onCBW(transfer);
    }
    else if (dev && transfer->actual_num_bytes == 13 && *(uint32_t *)transfer->data_buffer == 0x53425355) // CSW
    {
        dev->onCSW(transfer);
    }
    else
    {
        dev->onData(transfer);
        ESP_LOG_BUFFER_HEX("DATA", transfer->data_buffer, transfer->actual_num_bytes);
    }
}

USBmscDevice *USBmscDevice::instance = nullptr;

USBmscDevice *USBmscDevice::getInstance()
{
    return instance;
}

uint32_t USBmscDevice::getBlockCount(uint8_t lun)
{
    return block_count[lun];
}

uint16_t USBmscDevice::getBlockSize(uint8_t lun)
{
    return block_size[lun];
}

USBmscDevice::USBmscDevice(const usb_config_desc_t *config_desc, USBhost *host)
{
    esp_log_level_set("STATUS", ESP_LOG_ERROR);
    esp_log_level_set("EMIT", ESP_LOG_ERROR);
    esp_log_level_set("HEX", ESP_LOG_ERROR);
    esp_log_level_set("HEX_DATA", ESP_LOG_ERROR);
    esp_log_level_set("DATA", ESP_LOG_ERROR);

    _host = host;

    int offset = 0;
    for (size_t n = 0; n < config_desc->bNumInterfaces; n++)
    {
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, n, 0, &offset);
        const usb_ep_desc_t *ep = nullptr;

        if (intf->bInterfaceClass == 0x08)
        {
            if (intf->bNumEndpoints != 2)
                return;
            // ESP_LOGI("", "EP MSC.");
            for (size_t i = 0; i < intf->bNumEndpoints; i++)
            {
                int _offset = 0;
                ep = usb_parse_endpoint_descriptor_by_index(intf, i, config_desc->wTotalLength, &_offset);
                if (ep->bEndpointAddress & 0x80)
                {
                    ep_in = ep;
                }
                else
                {
                    ep_out = ep;
                }

                if (ep)
                {
                    printf("EP num: %d/%d, len: %d, ", i + 1, intf->bNumEndpoints, config_desc->wTotalLength);
                    printf("address: 0x%02x, EP max size: %d, dir: %s\n", ep->bEndpointAddress, ep->wMaxPacketSize, (ep->bEndpointAddress & 0x80) ? "IN" : "OUT");
                }
                else
                    ESP_LOGW("", "error to parse endpoint by index; EP num: %d/%d, len: %d", i + 1, intf->bNumEndpoints, config_desc->wTotalLength);
            }
            itf_num = n;
            esp_err_t err = usb_host_interface_claim(_host->clientHandle(), _host->deviceHandle(), itf_num, 0);
            ESP_LOGI("", "interface %d claim status: %d", itf_num, err);
        }
    }
    instance = this;
}

USBmscDevice::~USBmscDevice()
{
    esp_err_t err = usb_host_interface_release(_host->clientHandle(), _host->deviceHandle(), itf_num);
    ESP_LOGI("", "interface release status: %d", err);
}

esp_err_t USBmscDevice::dealocate(uint8_t _size)
{
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < 2; i++)
    {
        if (xfer_out[i])
        {
            err = usb_host_transfer_free(xfer_out[i]);
            if (ESP_OK != err)
            {
                ESP_LOGE("", "xfer_out : %d", err);
            }
        }
    }

    for (size_t i = 0; i < 2; i++)
    {
        if (xfer_in)
        {
            err = usb_host_transfer_free(xfer_in);
            if (ESP_OK != err)
            {
                ESP_LOGE("", "xfer_in : %d", err);
            }
        }
    }

    // err = usb_host_transfer_free(xfer_write);
    // if(ESP_OK != err){
    //     ESP_LOGE("", "xfer_write : %d", err);
    // }

    return err;
}


bool USBmscDevice::init()
{
    if (USBhostDevice::allocate(4096))
        ESP_LOGI("", "error to allocate transfers");

    xfer_out[_in]->callback = usb_transfer_cb;
    xfer_in->callback = usb_transfer_cb;
    xfer_read->callback = usb_transfer_cb;
    xfer_write->callback = usb_transfer_cb;
    xfer_ctrl->callback = usb_transfer_cb;

    xfer_out[_in]->bEndpointAddress = ep_out->bEndpointAddress;
    xfer_in->bEndpointAddress = ep_in->bEndpointAddress;
    xfer_read->bEndpointAddress = ep_out->bEndpointAddress;
    xfer_write->bEndpointAddress = ep_out->bEndpointAddress;
    xfer_ctrl->bEndpointAddress = 0;

    _getMaxLUN();

    return true;
}

void USBmscDevice::mount(char *path, uint8_t lun)
{
    if(lun > _luns) return;
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 1,
        .allocation_unit_size = block_size[lun]};
    esp_err_t err = vfs_fat_rawmsc_mount(path, &mount_config, lun);
    ESP_LOGI("", "VFS mount status: %d", err);
}

void USBmscDevice::unmount(char *path, uint8_t lun)
{
    if(lun > _luns) return;
    esp_err_t err = vfs_fat_rawmsc_unmount(path, lun);
    ESP_LOGI("", "VFS unmount status: %d", err);
}

void USBmscDevice::reset()
{
    event = 2;
    MSC_SCSI_REQ_INIT_RESET((usb_setup_packet_t *)xfer_ctrl->data_buffer, itf_num);
    xfer_ctrl->num_bytes = sizeof(usb_setup_packet_t);
    esp_err_t err = usb_host_transfer_submit_control(_host->clientHandle(), xfer_ctrl);
}

void USBmscDevice::_getMaxLUN()
{
    event = SCSI_CMD_MAX_LUN;
    MSC_SCSI_REQ_MAX_LUN((usb_setup_packet_t *)xfer_ctrl->data_buffer, itf_num);
    xfer_ctrl->num_bytes = sizeof(usb_setup_packet_t) + ((usb_setup_packet_t *)xfer_ctrl->data_buffer)->wLength;
    esp_err_t err = usb_host_transfer_submit_control(_host->clientHandle(), xfer_ctrl);
}

void USBmscDevice::inquiry()
{
    scsi_cmd10_t cbw = {};
    cbw.opcode = SCSI_CMD_INQUIRY;
    cbw.lba_1 = 36;

    event = cbw.opcode;

    mock_msc_scsi_init_cbw((msc_bulk_cbw_t *)xfer_out[_in]->data_buffer, true /*is_read*/, 0, 0x1234, (uint8_t *)&cbw, 36);

    xfer_out[_in]->num_bytes = sizeof(msc_bulk_cbw_t);

    esp_err_t err = usb_host_transfer_submit(xfer_out[_in]);
    if (err)
        ESP_LOGW("", "inquiry: 0x%02x => 0x%02x\n", err, xfer_out[_in]->bEndpointAddress);

    // xfer_in->callback = usb_transfer_cb; // TODO separate cb
    csw();
}

// FIXME the same as a write10
esp_err_t USBmscDevice::unitReady()
{
    scsi_cmd10_t cbw = {};
    cbw.opcode = SCSI_CMD_TEST_UNIT_READY;
    cbw.flags = 0;
    event = cbw.opcode;

    mock_msc_scsi_init_cbw((msc_bulk_cbw_t *)xfer_out[_in]->data_buffer, false /*is_read*/, _lun, 0x1234, (uint8_t *)&cbw, 0);

    xfer_out[_in]->num_bytes = sizeof(msc_bulk_cbw_t);

    esp_err_t err = usb_host_transfer_submit(xfer_out[_in]);
    printf("test ready: 0x%02x => 0x%02x\n", err, xfer_out[_in]->bEndpointAddress);
    // if(err)ESP_LOGW("", "test write10: 0x%02x => 0x%02x, sector: %d [%d][%d]\n", err, xfer_out[_in]->bEndpointAddress, offset, *(uint32_t*)&cbw.lba_3, __builtin_bswap32(*(uint32_t*)&cbw.lba_3));

    xfer_out[_in]->num_bytes = 0; // IDEA unit ready receive
    // memcpy(&xfer_out[_in]->data_buffer[0], buff, block_size[_lun]);
    err = usb_host_transfer_submit(xfer_out[_in]);
    ESP_LOGW("", "test unit ready data: 0x%02x", err);

    if (err)
    {
        ESP_LOGW("", "test unit ready data: 0x%02x", err);
        csw();
    }
    else
    {
        xTaskToNotify = xTaskGetCurrentTaskHandle();
        uint32_t pulNotificationValue;
        // xfer_write->callback = usb_transfer_cb;
        // xfer_write->num_bytes = block_size[_lun];
        // xfer_write->bEndpointAddress = ep_in->bEndpointAddress;

        // err = usb_host_transfer_submit(xfer_write);
        // if(1) ESP_LOGE("unit ready", "CSB err: %d\n", err);
        csw();

        if (xTaskNotifyWait(0, 0, &pulNotificationValue, 100) == pdTRUE)
        {
            return ESP_OK;
        }

        ESP_LOGE("", "test unit ready data: 0x%02x", ESP_FAIL);
    }
    return ESP_FAIL;
}

void USBmscDevice::_getCapacity(uint8_t lun)
{
    _lun = lun;
    scsi_cmd10_t cbw = {};
    cbw.opcode = SCSI_CMD_READ_CAPACITY_10;
    event = cbw.opcode;

    mock_msc_scsi_init_cbw((msc_bulk_cbw_t *)xfer_out[_in]->data_buffer, true /*is_read*/, lun, 0x1234, (uint8_t *)&cbw, 8);

    xfer_out[_in]->num_bytes = sizeof(msc_bulk_cbw_t);

    esp_err_t err = usb_host_transfer_submit(xfer_out[_in]);
    if (err)
        ESP_LOGW("", "test capacity: 0x%02x => 0x%02x\n", err, xfer_out[_in]->bEndpointAddress);

    csw();
}

void USBmscDevice::_setCapacity(uint32_t count, uint32_t _size)
{
    // IDEA
    // if (block_size[0] > _size)
    // {
    //     if(dealocate(_size))
    //         ESP_LOGI("", "error to deallocate transfers");
    //     if(allocate(4096))
    //         ESP_LOGI("", "error to allocate transfers");
    // }
    block_count[_lun] = count;
    block_size[_lun] = _size;
    ESP_LOGI("", "capacity => lun: %d count %d, size: %d", _lun, block_count[_lun], block_size[_lun]);
    csw();
}

void USBmscDevice::registerCallbacks(msc_transfer_cb_t cb)
{
    callbacks = cb;
}

//----------------------------------------------------------------------------------//

esp_err_t USBmscDevice::_read10(uint8_t lun, int offset, int num_sectors, uint8_t *buff)
{
    temp_buffer = buff;
    scsi_cmd10_t cbw = {};
    cbw.opcode = SCSI_CMD_READ_10;
    cbw.flags = 0;
    cbw.lba_3 = (offset >> 24);
    cbw.lba_2 = (offset >> 16);
    cbw.lba_1 = (offset >> 8);
    cbw.lba_0 = (offset >> 0);
    cbw.group = 0;
    cbw.len_1 = (num_sectors >> 8);
    cbw.len_0 = (num_sectors >> 0);
    cbw.control = 0;

    event = cbw.opcode;

    mock_msc_scsi_init_cbw((msc_bulk_cbw_t *)xfer_read->data_buffer, true /*is_read*/, lun, 0x1234, (uint8_t *)&cbw, num_sectors * block_size[lun]);

    xfer_read->num_bytes = sizeof(msc_bulk_cbw_t);

    esp_err_t err = usb_host_transfer_submit(xfer_read);
    if (err == 0x103){
        ESP_LOGW("", "test read10: 0x%02x => 0x%02x, sector: %d [%d][%d]\n", err, xfer_read->bEndpointAddress, offset, *(uint32_t *)&cbw.lba_3, __builtin_bswap32(*(uint32_t *)&cbw.lba_3));
        vTaskDelay(1);
        err = _read10(lun, offset, num_sectors, buff);
    }
    else
    {
        csw();
        xTaskToNotify = xTaskGetCurrentTaskHandle();
        uint32_t pulNotificationValue;
        if (xTaskNotifyWait(0, 0, &pulNotificationValue, 200) == pdTRUE)
        {
            return ESP_OK;
        }
    }

    return err;
}

esp_err_t USBmscDevice::_write10(uint8_t lun, int offset, int num_sectors, uint8_t *buff)
{

    scsi_cmd10_t cbw;
    cbw.opcode = SCSI_CMD_WRITE_10;
    cbw.flags = 0;
    cbw.lba_3 = (offset >> 24);
    cbw.lba_2 = (offset >> 16);
    cbw.lba_1 = (offset >> 8);
    cbw.lba_0 = (offset >> 0);
    cbw.group = 0;
    cbw.len_1 = (num_sectors >> 8);
    cbw.len_0 = (num_sectors >> 0);
    cbw.control = 0;

    event = cbw.opcode;

    mock_msc_scsi_init_cbw((msc_bulk_cbw_t *)xfer_write->data_buffer, false /*is_read*/, lun, 0x1234, (uint8_t *)&cbw, num_sectors * block_size[lun]);

    xfer_write->num_bytes = sizeof(msc_bulk_cbw_t); // block_size[lun] * num_sectors;

    esp_err_t err = usb_host_transfer_submit(xfer_write);
    if (err)
        ESP_LOGW("", "test write10: 0x%02x => 0x%02x, sector: %d [%d][%d]", err, xfer_write->bEndpointAddress, offset, *(uint32_t *)&cbw.lba_3, __builtin_bswap32(*(uint32_t *)&cbw.lba_3));

    xfer_read->num_bytes = block_size[_lun] * num_sectors;
    memcpy(&xfer_read->data_buffer[0], buff, block_size[_lun]);

    err = usb_host_transfer_submit(xfer_read);
    if (err)
    {
        ESP_LOGW("", "test write10 data: 0x%02x", err);
        csw();
    }
    else
    {
        xTaskToNotify = xTaskGetCurrentTaskHandle();
        uint32_t pulNotificationValue;
        csw();
        if (xTaskNotifyWait(0, 0, &pulNotificationValue, 50) == pdTRUE)
        {
            return ESP_OK;
        }

        ESP_LOGE("", "test write10 data: 0x%02x", ESP_FAIL);
    }
    return ESP_FAIL;
}

void USBmscDevice::csw()
{
    esp_err_t err;
    usb_transfer_t *_xfer_in;
    // usb_host_transfer_alloc(4096, 0, &_xfer_in);
    // _xfer_in->device_handle = _host->deviceHandle();
    // _xfer_in->context = this;
    // _xfer_in->bEndpointAddress = ep_in->bEndpointAddress;
    // _xfer_in->callback = usb_transfer_cb;
    // _xfer_in->num_bytes = usb_round_up_to_mps(sizeof(msc_bulk_csw_t), block_size[0]);
    // err = usb_host_transfer_submit(_xfer_in);
    // printf("new CSB err: %d\n", err);
    // if(err)usb_host_transfer_free(_xfer_in);

    xfer_in->num_bytes = usb_round_up_to_mps(sizeof(msc_bulk_csw_t), block_size[0]);

    err = usb_host_transfer_submit(xfer_in);
    if (err)
        printf("CSB err: %d\n", err);
}

void USBmscDevice::_emitEvent(host_event_t _event, usb_transfer_t *transfer)
{
    switch (_event)
    {
    case SCSI_CMD_MAX_LUN:
        ESP_LOGI("EMIT", "%s", "SCSI_CMD_MAX_LUN");
        _luns = transfer->data_buffer[8];
        _lun = 0;
        _getCapacity(_lun);

        if (callbacks.max_luns_cb)
        {
            callbacks.max_luns_cb(transfer);
        }
        break;
    case SCSI_CMD_TEST_UNIT_READY:
        ESP_LOGI("EMIT", "%s", "SCSI_CMD_TEST_UNIT_READY");
        // ets_delay_us(500);
        if (xTaskToNotify)
            xTaskNotify(xTaskToNotify, 0, eNoAction);
        break;
    case SCSI_CMD_INQUIRY:
        ESP_LOG_BUFFER_HEX("EMIT", transfer->data_buffer, transfer->actual_num_bytes);

        if (callbacks.inquiry_cb)
        {
            callbacks.inquiry_cb(transfer);
        }
        break;
    case SCSI_CMD_READ_CAPACITY_10:
    {
        ESP_LOGI("EMIT", "%s", "SCSI_CMD_READ_CAPACITY_10");
        if (_lun < _luns)
        {
            _getCapacity(++_lun);
        }
        else
        {
            inquiry();
            if (callbacks.capacity_cb)
            {
                callbacks.capacity_cb(transfer);
            }
        }
    }
    break;
    case SCSI_CMD_WRITE_10:
        ESP_LOGI("EMIT", "%s", "SCSI_CMD_WRITE_10");
        // ets_delay_us(500);
        if (xTaskToNotify)
            xTaskNotify(xTaskToNotify, 0, eNoAction);
        break;
    case SCSI_CMD_READ_10:
        ESP_LOGI("EMIT", "%s", "SCSI_CMD_READ_10");
        if (xTaskToNotify)
            xTaskNotify(xTaskToNotify, 0, eNoAction);
        break;

    default:
        ESP_LOGW("EMIT", "%d", _event);
        break;
    }

    // TODO emit event to user
}

uint8_t USBmscDevice::getMaxLUN()
{
    return _luns;
}

void USBmscDevice::format()
{
    scsi_cmd10_t cbw = {};
    cbw.opcode = SCSI_CMD_FORMAT_UNIT;
    // cbw.flags = _lun | 0xE8;
    // cbw.lba_3 = (offset >> 24);
    // cbw.lba_2 = (offset >> 16);
    // cbw.lba_1 = (offset >> 8);
    // cbw.lba_0 = (offset >> 0);
    // cbw.group = 0;
    // cbw.len_1 = (num_sectors >> 8);
    // cbw.len_0 = (num_sectors >> 0);
    // cbw.control = 0;
    event = cbw.opcode;

    mock_msc_scsi_init_cbw((msc_bulk_cbw_t *)xfer_out[_in]->data_buffer, true /*is_read*/, _lun, 0x1234, (uint8_t *)&cbw, 8);

    xfer_out[_in]->num_bytes = sizeof(msc_bulk_cbw_t);

    esp_err_t err = usb_host_transfer_submit(xfer_out[_in]);
    printf("test capacity: 0x%02x => 0x%02x\n", err, xfer_out[_in]->bEndpointAddress);
    csw();
}

//-------------------------------------- Private ----------------------------------//

void USBmscDevice::onCSW(usb_transfer_t *transfer)
{
    _emitEvent(event, transfer);
    // usb_host_transfer_free(transfer);
    if (callbacks.csw_cb)
    {
        callbacks.csw_cb(transfer);
    }
}

void USBmscDevice::onCBW(usb_transfer_t *transfer)
{
    ESP_LOG_BUFFER_HEX("HEX", transfer->data_buffer, transfer->actual_num_bytes);
    if (callbacks.cbw_cb)
    {
        callbacks.cbw_cb(transfer);
    }
}

void USBmscDevice::onData(usb_transfer_t *transfer)
{
    switch (event)
    {
    case SCSI_CMD_INQUIRY:
    {
        ESP_LOG_BUFFER_HEX("HEX", transfer->data_buffer, transfer->actual_num_bytes);
        csw();
    }
    break;
    case SCSI_CMD_READ_10:
    {
        memcpy(temp_buffer, transfer->data_buffer, block_size[_lun]);
        csw();
    }
    break;
    case SCSI_CMD_WRITE_10:
        break;
    case SCSI_CMD_READ_CAPACITY_10:
    {
        uint32_t block_count = __builtin_bswap32(*(uint32_t *)&transfer->data_buffer[0]);
        uint32_t block_size = __builtin_bswap32(*(uint32_t *)&transfer->data_buffer[4]);

        _setCapacity(block_count, block_size);
    }
    break;

    default:
    {
        ESP_LOGE("", "event: %d", event);
        ESP_LOG_BUFFER_HEX("HEX", transfer->data_buffer, 16);
    }
    break;
    }

    if (callbacks.data_cb)
    {
        callbacks.data_cb(transfer);
    }
}




