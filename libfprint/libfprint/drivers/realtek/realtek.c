/*
 * Copyright (C) 2022-2023 Realtek Corp.
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "realtek"

#include "drivers_api.h"

#include "fpi-byte-reader.h"

#include "realtek.h"

G_DEFINE_TYPE (FpiDeviceRealtek, fpi_device_realtek, FP_TYPE_DEVICE)

/* The device-bound 49-byte commit blob (identical for all enrollments;
 * decoded from the Windows enrollment capture). */
static const guint8 verimark_commit_blob[49] = {
  0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x15, 0x00, 0x00, 0x00,
  0xf0, 0xf1, 0xe8, 0xe1, 0x5c, 0x30, 0xc9, 0xd5, 0x0d, 0xe5, 0x43, 0xaf,
  0xeb, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfb, 0x44,
  0x99, 0x98, 0xfc, 0xbc, 0x3c, 0xe7, 0xad, 0x4b, 0xbc, 0xfc, 0xe7, 0x3c,
  0xad, 0x4a
};
static const FpIdEntry id_table[] = {
  { .vid = 0x0bda,  .pid = 0x5813,  },
  { .vid = 0x0bda,  .pid = 0x5816,  },
  { .vid = 0x2541,  .pid = 0xfa03,  },
  { .vid = 0x3274,  .pid = 0x9003,  },
  /* Kensington VeriMark Desktop 2.0 (K64741WW) — Realtek RTS58xx sensor
   * under Kensington VID; EP layout matches (bulk out 0x01 / in 0x82). */
  { .vid = 0x047d,  .pid = 0x8228,  },
  { .vid = 0,  .pid = 0,  .driver_data = 0 },   /* terminating entry */
};

static gboolean
parse_print_data (GVariant      *data,
                  guint8        *finger,
                  const guint8 **user_id,
                  gsize         *user_id_len)
{
  g_autoptr(GVariant) user_id_var = NULL;

  g_return_val_if_fail (data, FALSE);
  g_return_val_if_fail (finger, FALSE);
  g_return_val_if_fail (user_id, FALSE);
  g_return_val_if_fail (user_id_len, FALSE);

  *user_id = NULL;
  *user_id_len = 0;
  *finger = 0;

  if (!g_variant_check_format_string (data, "(y@ay)", FALSE))
    return FALSE;

  g_variant_get (data,
                 "(y@ay)",
                 finger,
                 &user_id_var);

  *user_id = g_variant_get_fixed_array (user_id_var, user_id_len, 1);

  /* The device records carry no uid; an empty one is valid. */
  if (*user_id_len > DEFAULT_UID_LEN)
    return FALSE;

  if (*user_id_len > 0 && (*user_id[0] == '\0' || *user_id[0] == ' '))
    return FALSE;

  if (*finger < 0xf5 || *finger > 0xfe)
    return FALSE;

  return TRUE;
}

static void
fp_cmd_ssm_done_data_free (CommandData *data)
{
  g_free (data);
}

/* data callbacks */

static void
fp_task_ssm_generic_cb (FpiDeviceRealtek *self,
                        uint8_t          *buffer_in,
                        GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_get_device_info_cb (FpiDeviceRealtek *self,
                       uint8_t          *buffer_in,
                       GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  /* The device-info reply's first byte (0x39 on 047d:8228) is not the
   * template record length — wire captures show the device returns
   * template_num × 35 B records, so keep the stock size. */
  self->template_len = TEMPLATE_LEN_COMMON;

  fp_dbg ("device-info: %02x %02x %02x %02x %02x %02x %02x %02x",
          buffer_in[0], buffer_in[1], buffer_in[2], buffer_in[3],
          buffer_in[4], buffer_in[5], buffer_in[6], buffer_in[7]);

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_enroll_commit_cb (FpiDeviceRealtek *self,
                     uint8_t          *buffer_in,
                     GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_ENROLL_NUM_STATES);
}

static void
fp_finish_capture_cb (FpiDeviceRealtek *self,
                      uint8_t          *buffer_in,
                      GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  /* We hope this polling CMD can be completed before the action is cancelled */
  GCancellable *cancellable = fpi_device_get_cancellable (FP_DEVICE (self));
  if (g_cancellable_is_cancelled (cancellable))
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Action is cancelled!"));
      return;
    }

  gint capture_status = buffer_in[0];
  if (capture_status == 0)
    {
      fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                               FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NEEDED);
      fpi_ssm_next_state (self->task_ssm);
    }
  else
    {
      fpi_ssm_jump_to_state (self->task_ssm,
                             fpi_ssm_get_cur_state (self->task_ssm));
    }
}

static void
fp_accept_sample_cb (FpiDeviceRealtek *self,
                     uint8_t          *buffer_in,
                     GError           *error)
{
  fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                           FP_FINGER_STATUS_NONE,
                                           FP_FINGER_STATUS_PRESENT);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  gint in_status = buffer_in[0];

  if (self->fp_purpose != FP_RTK_PURPOSE_ENROLL)
    {
      /* verify or identify purpose process */
      fpi_ssm_next_state (self->task_ssm);
      return;
    }
  else
    {
      /* enroll purpose process */
      if (in_status == FP_RTK_CMD_ERR)
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Command error!"));
          return;
        }

      if (self->enroll_stage < self->max_enroll_stage)
        {
          if (in_status == FP_RTK_SUCCESS)
            {
              self->enroll_stage++;
              fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_stage, NULL, NULL);
              fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_ENROLL_CAPTURE);
            }
          else if (in_status == FP_RTK_MATCH_FAIL)
            {
              fpi_ssm_mark_failed (self->task_ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                             "InStatus invalid!"));
            }
          else
            {
              fpi_device_enroll_progress (FP_DEVICE (self),
                                          self->enroll_stage,
                                          NULL,
                                          fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));

              fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_ENROLL_CAPTURE);
            }
          return;
        }

      if (in_status != FP_RTK_SUCCESS)
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                         "Enrollment failed (status 0x%02x)",
                                                         in_status));
          return;
        }

      fpi_ssm_next_state (self->task_ssm);
    }
}

static FpPrint *
fp_print_from_data (FpiDeviceRealtek *self, uint8_t *buffer)
{
  FpPrint *print;
  GVariant *data;
  GVariant *uid;
  guint finger;

  /* The 52-byte on-device records carry no host uid string: after the
   * valid marker they are [0xF5+slot] 01 <48-byte body>. Both the template
   * list (at SUBFACTOR_OFFSET) and the verify match reply (past the status
   * byte) start on the sub-factor byte. The saved enrollment print must be
   * byte-identical to what this builds, or fprintd's storage sync
   * (fp_print_equal) deletes it as "unknown to device". */
  finger = *(buffer);

  print = fp_print_new (FP_DEVICE (self));

  uid = g_variant_new_array (G_VARIANT_TYPE_BYTE, NULL, 0);

  data = g_variant_new ("(y@ay)",
                        finger,
                        uid);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);

  return print;
}

static void
fp_identify_feature_cb (FpiDeviceRealtek *self,
                        uint8_t          *buffer_in,
                        GError           *error)
{
  FpDevice *device = FP_DEVICE (self);
  FpPrint *match = NULL;
  FpPrint *print = NULL;
  FpiDeviceAction current_action;

  g_autoptr(GPtrArray) templates = NULL;
  gboolean found = FALSE;

  current_action = fpi_device_get_current_action (device);

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  gint in_status = buffer_in[0];
  if (in_status == FP_RTK_CMD_ERR)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Command error!"));
      return;
    }

  /* Only the low capture-quality codes ask for a new scan. The
   * identify/verify reply uses 0x09/0x0b as its no-match codes (values
   * that collide with TOO_SHORT/MATCH_FAIL) and 0x00 for a match; those
   * must fall through to the no-match reporting below. */
  if (in_status >= FP_RTK_TOO_HIGH && in_status < FP_RTK_TOO_SHORT)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return;
    }

  if (in_status == FP_RTK_SUCCESS)
    {
      match = fp_print_from_data (self, buffer_in + 1);

      if (current_action == FPI_DEVICE_ACTION_VERIFY)
        {
          templates = g_ptr_array_sized_new (1);
          fpi_device_get_verify_data (device, &print);
          g_ptr_array_add (templates, print);
        }
      else
        {
          fpi_device_get_identify_data (device, &templates);
          g_ptr_array_ref (templates);
        }

      for (gint cnt = 0; cnt < templates->len; cnt++)
        {
          print = g_ptr_array_index (templates, cnt);

          if (fp_print_equal (print, match))
            {
              found = TRUE;
              break;
            }
        }

      if (found)
        {
          if (current_action == FPI_DEVICE_ACTION_VERIFY)
            {
              fpi_device_verify_report (device, FPI_MATCH_SUCCESS, match, error);
              fpi_ssm_next_state (self->task_ssm);
            }
          else
            {
              fpi_device_identify_report (device, print, match, error);
              fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_VERIFY_NUM_STATES);
            }
          return;
        }
    }

  if (!found)
    {
      if (current_action == FPI_DEVICE_ACTION_VERIFY)
        fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, error);
      else
        fpi_device_identify_report (device, NULL, NULL, error);

      fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_VERIFY_NUM_STATES);
    }
}

static void
fp_get_delete_pos_cb (FpiDeviceRealtek *self,
                      uint8_t          *buffer_in,
                      GError           *error)
{
  FpPrint *print = NULL;

  g_autoptr(GVariant) data = NULL;
  gsize user_id_len = 0;
  const guint8 *user_id;
  guint8 finger;
  gboolean found = FALSE;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_device_get_delete_data (FP_DEVICE (self), &print);
  g_object_get (print, "fpi-data", &data, NULL);

  if (!parse_print_data (data, &finger, &user_id, &user_id_len))
    {
      fpi_device_delete_complete (FP_DEVICE (self),
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  /* The records carry no uid; locate the slot by its sub-factor byte. */
  for (gint i = 0; i < self->template_num; i++)
    {
      if (buffer_in[i * self->template_len] != 0)
        {
          if (buffer_in[i * self->template_len + SUBFACTOR_OFFSET] == finger)
            {
              self->pos_index = i;
              found = TRUE;
              break;
            }
        }
    }

  if (!found)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Get template position failed!"));
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_get_enroll_num_cb (FpiDeviceRealtek *self,
                      uint8_t          *buffer_in,
                      GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  self->template_num = buffer_in[1];

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_verify_get_template_cb (FpiDeviceRealtek *self,
                           uint8_t          *buffer_in,
                           GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_enroll_get_template_cb (FpiDeviceRealtek *self,
                           uint8_t          *buffer_in,
                           GError           *error)
{
  g_autofree guchar *seq_list = NULL;
  gboolean found = FALSE;

  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  for (gint i = 0; i < self->template_num; i++)
    {
      if (buffer_in[i * self->template_len] == 0)
        {
          self->pos_index = i;
          found = TRUE;
          break;
        }
    }

  if (!found)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "No free template was found!"));
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fp_check_duplicate_cb (FpiDeviceRealtek *self,
                       uint8_t          *buffer_in,
                       GError           *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  gint in_status = buffer_in[0];
  if (in_status == FP_RTK_CMD_ERR)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Command error!"));
      return;
    }

  if (in_status == FP_RTK_SUCCESS)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_DUPLICATE,
                                                     "Current fingerprint is duplicate!"));
    }
  else if (in_status == FP_RTK_MATCH_FAIL)
    {
      fpi_ssm_next_state (self->task_ssm);
    }
  else
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                     "InStatus invalid!"));
    }
}

static void
fp_list_cb (FpiDeviceRealtek *self,
            uint8_t          *buffer_in,
            GError           *error)
{
  gboolean found = FALSE;

  g_autoptr(GPtrArray) list_result = NULL;

  if (error)
    {
      fpi_device_list_complete (FP_DEVICE (self), NULL, error);
      return;
    }

  list_result = g_ptr_array_new_with_free_func (g_object_unref);

  for (gint i = 0; i < self->template_num; i++)
    {
      if (buffer_in[i * self->template_len] != 0)
        {
          FpPrint *print = NULL;
          print = fp_print_from_data (self, buffer_in + i * self->template_len + SUBFACTOR_OFFSET);
          g_ptr_array_add (list_result, g_object_ref_sink (print));
          found = TRUE;
        }
    }

  if (!found)
    {
      fpi_device_list_complete (FP_DEVICE (self),
                                g_steal_pointer (&list_result),
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                                          "Database is empty"));
      return;
    }

  fp_info ("Query templates complete!");
  fpi_device_list_complete (FP_DEVICE (self),
                            g_steal_pointer (&list_result),
                            NULL);
}

static void
fp_clear_storage_cb (FpiDeviceRealtek *self,
                     uint8_t          *buffer_in,
                     GError           *error)
{
  FpDevice *device = FP_DEVICE (self);

  if (error)
    {
      fpi_device_clear_storage_complete (device, error);
      return;
    }

  fp_info ("Successfully cleared storage");
  fpi_device_clear_storage_complete (device, NULL);
}


static gint
parse_status (guint8 *buffer, gint status_type)
{
  switch (status_type)
    {
    case FP_RTK_MSG_NO_STATUS:
      return 0;
      break;

    case FP_RTK_MSG_DEFAULT:
      return buffer[0];
      break;

    default:
      return 1;
      break;
    }
}

static void
fp_cmd_receive_cb (FpiUsbTransfer *transfer,
                   FpDevice       *device,
                   gpointer        user_data,
                   GError         *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  CommandData *data = user_data;
  gint ssm_state = 0;
  gint status_flag = 1;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (data == NULL)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  ssm_state = fpi_ssm_get_cur_state (transfer->ssm);

  /* skip zero length package */
  if (transfer->actual_length == 0)
    {
      fpi_ssm_jump_to_state (transfer->ssm, ssm_state);
      return;
    }

  /* get data */
  if (ssm_state == FP_RTK_CMD_TRANS_DATA)
    {
      g_autofree guchar *read_buf = NULL;

      if ((gsize) transfer->actual_length < self->trans_data_len)
        {
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Truncated data received"));
          return;
        }

      read_buf = g_malloc0 (sizeof (guchar) * (self->trans_data_len));
      memcpy (read_buf, transfer->buffer, self->trans_data_len);
      self->read_data = g_steal_pointer (&read_buf);

      fpi_ssm_next_state (transfer->ssm);
      return;
    }

  /* get status */
  status_flag = parse_status (transfer->buffer, self->message_type);
  if (status_flag != 0)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Status check failed"));
      return;
    }

  if (data->callback)
    data->callback (self, self->read_data, NULL);

  if (self->read_data)
    g_clear_pointer (&self->read_data, g_free);

  fpi_ssm_mark_completed (transfer->ssm);
}

static void
fp_cmd_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *transfer = NULL;
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_CMD_SEND:
      if (self->cmd_transfer)
        {
          self->cmd_transfer->ssm = ssm;
          fpi_usb_transfer_submit (g_steal_pointer (&self->cmd_transfer),
                                   CMD_TIMEOUT,
                                   NULL,
                                   fpi_ssm_usb_transfer_cb,
                                   NULL);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case FP_RTK_CMD_TRANS_DATA:
      if (self->cmd_type == FP_RTK_CMD_BULK_ONLY)
        {
          fpi_ssm_jump_to_state (ssm, FP_RTK_CMD_GET_STATUS);
          break;
        }

      if (self->cmd_type == FP_RTK_CMD_BULK_WRITE)
        {
          if (self->data_transfer)
            {
              self->data_transfer->ssm = ssm;
              fpi_usb_transfer_submit (g_steal_pointer (&self->data_transfer),
                                       DATA_TIMEOUT,
                                       NULL,
                                       fpi_ssm_usb_transfer_cb,
                                       NULL);
            }
          else
            {
              fpi_ssm_next_state (ssm);
            }
        }
      else  /* CMD_READ */
        {
          transfer = fpi_usb_transfer_new (dev);
          transfer->ssm = ssm;
          fpi_usb_transfer_fill_bulk (transfer, EP_IN, EP_IN_MAX_BUF_SIZE);

          fpi_usb_transfer_submit (transfer,
                                   self->cmd_cancellable ? 0 : DATA_TIMEOUT,
                                   self->cmd_cancellable ? fpi_device_get_cancellable (dev) : NULL,
                                   fp_cmd_receive_cb,
                                   fpi_ssm_get_data (ssm));
        }
      break;

    case FP_RTK_CMD_GET_STATUS:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_bulk (transfer, EP_IN, EP_IN_MAX_BUF_SIZE);
      fpi_usb_transfer_submit (transfer,
                               STATUS_TIMEOUT,
                               NULL,
                               fp_cmd_receive_cb,
                               fpi_ssm_get_data (ssm));
      break;
    }
}

static void
fp_cmd_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);
  CommandData *data = fpi_ssm_get_data (ssm);

  self->cmd_ssm = NULL;

  if (error)
    {
      if (data->callback)
        data->callback (self, NULL, error);
      else
        g_error_free (error);
    }
}

static FpiUsbTransfer *
prepare_bulk_transfer (FpDevice      *dev,
                       guint8        *data,
                       gsize          data_len,
                       GDestroyNotify free_func)
{
  g_autoptr(FpiUsbTransfer) transfer = NULL;

  g_return_val_if_fail (data || data_len == 0, NULL);

  transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk_full (transfer,
                                   EP_OUT,
                                   data,
                                   data_len,
                                   free_func);

  return g_steal_pointer (&transfer);
}


static void
fp_ctrl_cmd_cb (FpiUsbTransfer *transfer,
                FpDevice       *device,
                gpointer        user_data,
                GError         *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  g_autofree CommandData *data = g_steal_pointer (&user_data);

  g_return_if_fail (data != NULL);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->direction == G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE)
    {
      if (data->callback)
        data->callback (self, NULL, NULL);
    }
  else
    {
      if (transfer->actual_length == 0)
        {
          fp_info ("Control transfer receive data failed!");
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }

      if (data->callback)
        data->callback (self, transfer->buffer, NULL);
    }
}

static void
rtk_send_ctrl_cmd (FpiDeviceRealtek    *self,
                   struct rtk_cmd_ctrl *ctrl_cmd,
                   guint8              *cmd_data,
                   SynCmdMsgCallback    callback)
{
  FpiUsbTransfer *transfer = NULL;
  g_autofree CommandData *data = g_new0 (CommandData, 1);

  data->callback = callback;

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_control (transfer,
                                 ctrl_cmd->direction,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 ctrl_cmd->request,
                                 ctrl_cmd->value,
                                 ctrl_cmd->index,
                                 ctrl_cmd->len);

  transfer->ssm = self->task_ssm;
  if (ctrl_cmd->direction == G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST &&
      cmd_data != NULL && ctrl_cmd->len != 0)
    memcpy (transfer->buffer, cmd_data, ctrl_cmd->len);

  fpi_usb_transfer_submit (transfer,
                           CMD_TIMEOUT,
                           NULL,
                           fp_ctrl_cmd_cb,
                           g_steal_pointer (&data));
}

static void
rtk_sensor_bulk_cmd (FpiDeviceRealtek *self,
                     guint8           *cmd,
                     guint8           *trans_data,
                     FpRtkMsgType      message_type,
                     gboolean          bwait_data_delay,
                     SynCmdMsgCallback callback)
{
  g_autoptr(FpiUsbTransfer) cmd_transfer = NULL;
  g_autoptr(FpiUsbTransfer) data_transfer = NULL;
  CommandData *data = g_new0 (CommandData, 1);

  self->cmd_type = GET_BULK_CMD_TYPE (cmd[0]);
  self->message_type = message_type;
  self->trans_data_len = GET_TRANS_DATA_LEN (cmd[11], cmd[10]);
  self->cmd_cancellable = bwait_data_delay;

  cmd_transfer = prepare_bulk_transfer (FP_DEVICE (self), cmd, FP_RTK_CMD_BULK_TOTAL_LEN, NULL);
  self->cmd_transfer = g_steal_pointer (&cmd_transfer);

  if ((self->cmd_type == FP_RTK_CMD_BULK_WRITE) && trans_data)
    {
      data_transfer = prepare_bulk_transfer (FP_DEVICE (self), trans_data, self->trans_data_len, g_free);
      self->data_transfer = g_steal_pointer (&data_transfer);
    }

  self->cmd_ssm = fpi_ssm_new (FP_DEVICE (self),
                               fp_cmd_run_state,
                               FP_RTK_CMD_NUM_STATES);

  data->callback = callback;
  fpi_ssm_set_data (self->cmd_ssm, data, (GDestroyNotify) fp_cmd_ssm_done_data_free);

  fpi_ssm_start (self->cmd_ssm, fp_cmd_ssm_done);
}

static void
fp_verify_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);

  fp_info ("Verify complete!");

  if (error && error->domain == FP_DEVICE_RETRY)
    {
      if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
        fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, g_steal_pointer (&error));
      else
        fpi_device_identify_report (dev, NULL, NULL, g_steal_pointer (&error));
    }

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    fpi_device_verify_complete (dev, error);
  else
    fpi_device_identify_complete (dev, error);

  self->task_ssm = NULL;
}

static void
fp_enroll_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);
  FpPrint *print = NULL;

  self->commit_retry = FALSE;

  fp_info ("Enrollment complete!");

  if (error)
    {
      fpi_device_enroll_complete (dev, NULL, error);
      self->task_ssm = NULL;
      return;
    }

  fpi_device_get_enroll_data (FP_DEVICE (self), &print);
  fpi_device_enroll_complete (FP_DEVICE (self), g_object_ref (print), NULL);
  self->task_ssm = NULL;
}

static void
fp_init_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);

  fp_info ("Init complete!");

  fpi_device_open_complete (dev, error);
  self->task_ssm = NULL;
}

static void
fp_delete_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (dev);

  fp_info ("Delete print complete!");

  fpi_device_delete_complete (dev, error);
  self->task_ssm = NULL;
}

static void
fp_verify_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_VERIFY_GET_TEMPLATE:
      if (self->template_num <= 0)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "No templates enrolled"));
          break;
        }

      co_get_template.data_len[0] = GET_LEN_L (self->template_len * self->template_num);
      co_get_template.data_len[1] = GET_LEN_H (self->template_len * self->template_num);
      cmd_buf = (guint8 *) &co_get_template;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_verify_get_template_cb);
      break;

    case FP_RTK_VERIFY_CAPTURE:
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);

      co_start_capture.param[0] = 0x02;
      co_start_capture.param[1] = 0x02;
      co_finish_capture.param[0] = 0x02;
      co_finish_capture.param[1] = 0x02;
      cmd_buf = (guint8 *) &co_start_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_VERIFY_FINISH_CAPTURE:
      cmd_buf = (guint8 *) &co_finish_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_finish_capture_cb);
      break;

    case FP_RTK_VERIFY_ACCEPT_SAMPLE:
      co_accept_sample.param[0] = 0x02;
      co_accept_sample.param[1] = 0x00;
      cmd_buf = (guint8 *) &co_accept_sample;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_accept_sample_cb);
      break;

    case FP_RTK_VERIFY_INDENTIFY_FEATURE:
      nor_identify_feature.param[0] = 0x00;
      nor_identify_feature.param[1] = 0x02;
      cmd_buf = (guint8 *) &nor_identify_feature;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_identify_feature_cb);
      break;

    case FP_RTK_VERIFY_UPDATE_TEMPLATE:
      /* The stock driver's 05 11 "update template" triggers a ~4 s flash
       * erase + firmware reboot on this firmware and wipes the committed
       * templates (see FINDINGS). Never send it — just finish the SSM. */
      fpi_ssm_jump_to_state (self->task_ssm, FP_RTK_VERIFY_NUM_STATES);
      break;

    case FP_RTK_VERIFY_CANCEL_CAPTURE:
      co_cancel_capture.param[0] = self->fp_purpose;
      cmd_buf = (guint8 *) &co_cancel_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;
    }
}

/* Synchronously switch the sensor's select_os mode (best-effort; used
 * before retrying commit variants in the alternate mode). */
static void
commit_send_select_mode (FpiDeviceRealtek *self, guint8 mode)
{
  guint8 buf[12] = { 0x05, 0x13, mode, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  g_autoptr(FpiUsbTransfer) cmd = NULL;
  g_autoptr(FpiUsbTransfer) rsp = NULL;
  g_autoptr(GError) err = NULL;

  cmd = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_bulk_full (cmd, EP_OUT, buf, sizeof (buf), NULL);
  if (!fpi_usb_transfer_submit_sync (cmd, CMD_TIMEOUT, &err))
    {
      fp_warn ("select_os(%u) write failed: %s", mode, err->message);
      return;
    }

  rsp = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_bulk (rsp, EP_IN, EP_IN_MAX_BUF_SIZE);
  if (!fpi_usb_transfer_submit_sync (rsp, STATUS_TIMEOUT, &err))
    {
      fp_warn ("select_os(%u) status read failed: %s", mode, err->message);
      return;
    }

  fp_info ("select_os(%u) status: %02x %02x %02x %02x %02x", mode,
           rsp->buffer[0], rsp->buffer[1], rsp->buffer[2],
           rsp->buffer[3], rsp->buffer[4]);
}

static void
fp_enroll_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  g_autofree gchar *user_id = NULL;
  g_autofree guint8 *payload = NULL;
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  FpPrint *print = NULL;
  guint8 *cmd_buf = NULL;
  GVariant *uid = NULL;
  GVariant *data = NULL;
  guint finger;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_ENROLL_GET_TEMPLATE:
      if (self->template_num <= 0)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "No templates enrolled"));
          break;
        }

      co_get_template.data_len[0] = GET_LEN_L (self->template_len * self->template_num);
      co_get_template.data_len[1] = GET_LEN_H (self->template_len * self->template_num);

      cmd_buf = (guint8 *) &co_get_template;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_enroll_get_template_cb);
      break;

    case FP_RTK_ENROLL_BEGIN_POS:
      nor_enroll_begin.param[0] = self->pos_index;
      cmd_buf = (guint8 *) &nor_enroll_begin;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_ENROLL_CAPTURE:
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);

      co_start_capture.param[0] = 0x04;
      co_start_capture.param[1] = 0x02;
      co_finish_capture.param[0] = 0x04;
      co_finish_capture.param[1] = 0x02;
      cmd_buf = (guint8 *) &co_start_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_ENROLL_FINISH_CAPTURE:
      cmd_buf = (guint8 *) &co_finish_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_finish_capture_cb);
      break;

    case FP_RTK_ENROLL_ACCEPT_SAMPLE:
      co_accept_sample.param[0] = self->fp_purpose;
      cmd_buf = (guint8 *) &co_accept_sample;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_accept_sample_cb);
      break;

    case FP_RTK_ENROLL_CHECK_DUPLICATE:
      cmd_buf = (guint8 *) &co_check_duplicate;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_NO_STATUS, 0, fp_check_duplicate_cb);
      break;

    case FP_RTK_ENROLL_COMMIT:
      gint payload_len;
      guint8 commit_class, commit_sub, commit_param;

      if (!self->commit_retry)
        {
          self->commit_retry = TRUE;
          self->commit_variant = 0;
        }

      /* The Kensington/Atans store (decoded from the Windows capture):
       * 85 21 [0xF5+slot] dlen=49 + the device-bound 49-byte blob. */
      commit_class = 0x85;
      commit_sub = 0x21;
      commit_param = 0xf5 + (self->pos_index & 0x0f);
      payload_len = 49;

      fpi_device_get_enroll_data (device, &print);
      user_id = fpi_print_generate_user_id (print);

      if (payload_len > 0)
        {
          payload = g_malloc0 (payload_len);
          memcpy (payload, verimark_commit_blob,
                  MIN ((gsize) payload_len, sizeof (verimark_commit_blob)));
        }

      /* Store the on-device sub-factor in the print data; the device
       * records carry no uid, and the saved print must equal the record
       * the list/verify paths report (fp_print_equal is a byte compare). */
      finger = commit_param;
      uid = g_variant_new_array (G_VARIANT_TYPE_BYTE, NULL, 0);
      data = g_variant_new ("(y@ay)",
                            finger,
                            uid);

      if (self->commit_variant == 0)
        {
          fpi_print_set_type (print, FPI_PRINT_RAW);
          fpi_print_set_device_stored (print, TRUE);
          g_object_set (print, "fpi-data", data, NULL);
          g_object_set (print, "description", user_id, NULL);
        }

      g_debug ("user_id: %s, finger: 0x%x", user_id, finger);

      memset (self->commit_cmd_buf, 0, sizeof (self->commit_cmd_buf));
      self->commit_cmd_buf[0] = commit_class;
      self->commit_cmd_buf[1] = commit_sub;
      self->commit_cmd_buf[2] = commit_param;
      self->commit_cmd_buf[10] = GET_LEN_L (payload_len);
      self->commit_cmd_buf[11] = GET_LEN_H (payload_len);

      fp_info ("enroll commit attempt %d: %02x %02x param=%02x dlen=%d",
               self->commit_variant, commit_class, commit_sub,
               commit_param, payload_len);

      rtk_sensor_bulk_cmd (self, self->commit_cmd_buf,
                           payload_len > 0 ? g_steal_pointer (&payload) : NULL,
                           FP_RTK_MSG_DEFAULT, 0, fp_enroll_commit_cb);
      break;

    case FP_RTK_ENROLL_CANCEL_CAPTURE:
      co_cancel_capture.param[0] = self->fp_purpose;
      cmd_buf = (guint8 *) &co_cancel_capture;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
    }
}

static void
fp_init_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_INIT_GET_DEVICE_INFO:
      rtk_send_ctrl_cmd (self, &get_device_info, NULL, fp_get_device_info_cb);
      break;

    case FP_RTK_INIT_SELECT_OS:
      /* select_os mode selects the on-device template database:
       * mode 1 is the SDCP-protected Windows Hello database (all
       * unauthenticated database operations are silently discarded),
       * mode 0 is the open database the stock driver's commit writes
       * to. Verified on Kensington 047d:8228 (Atans firmware). */
      co_select_system.param[0] = 0x00;
      cmd_buf = (guint8 *) &co_select_system;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;

    case FP_RTK_INIT_GET_ENROLL_NUM:
      cmd_buf = (guint8 *) &co_get_enroll_num;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_get_enroll_num_cb);
      break;
    }
}

static void
fp_delete_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_RTK_DELETE_GET_POS:
      if (self->template_num <= 0)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "No templates enrolled"));
          break;
        }

      co_get_template.data_len[0] = GET_LEN_L (self->template_len * self->template_num);
      co_get_template.data_len[1] = GET_LEN_H (self->template_len * self->template_num);

      cmd_buf = (guint8 *) &co_get_template;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_get_delete_pos_cb);
      break;

    case FP_RTK_DELETE_PRINT:
      co_delete_record.param[0] = self->pos_index;
      cmd_buf = (guint8 *) &co_delete_record;
      rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_task_ssm_generic_cb);
      break;
    }
}


static void
identify_verify (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  FpiDeviceAction current_action;

  G_DEBUG_HERE ();
  current_action = fpi_device_get_current_action (device);

  g_assert (current_action == FPI_DEVICE_ACTION_VERIFY ||
            current_action == FPI_DEVICE_ACTION_IDENTIFY);

  self->fp_purpose = FP_RTK_PURPOSE_IDENTIFY;

  g_assert (!self->task_ssm);

  self->task_ssm = fpi_ssm_new_full (device,
                                     fp_verify_sm_run_state,
                                     FP_RTK_VERIFY_NUM_STATES,
                                     FP_RTK_VERIFY_CANCEL_CAPTURE,
                                     "Verify & Identify");

  fpi_ssm_start (self->task_ssm, fp_verify_ssm_done);
}

static void
enroll (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);

  G_DEBUG_HERE ();
  self->enroll_stage = 0;
  self->fp_purpose = FP_RTK_PURPOSE_ENROLL;

  g_assert (!self->task_ssm);

  self->task_ssm = fpi_ssm_new_full (device,
                                     fp_enroll_sm_run_state,
                                     FP_RTK_ENROLL_NUM_STATES,
                                     FP_RTK_ENROLL_CANCEL_CAPTURE,
                                     "Enroll");

  fpi_ssm_start (self->task_ssm, fp_enroll_ssm_done);
}

static void
dev_probe (FpDevice *device)
{
  GUsbDevice *usb_dev;
  GError *error = NULL;
  g_autofree gchar *product = NULL;
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);

  G_DEBUG_HERE ();
  /* Claim usb interface */
  usb_dev = fpi_device_get_usb_device (device);
  if (!g_usb_device_open (usb_dev, &error))
    {
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (!g_usb_device_reset (usb_dev, &error))
    {
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  product = g_usb_device_get_string_descriptor (usb_dev,
                                                g_usb_device_get_product_index (usb_dev),
                                                &error);

  if (product)
    fp_dbg ("Device name: %s", product);

  self->max_enroll_stage = MAX_ENROLL_SAMPLES;
  fpi_device_set_nr_enroll_stages (device, self->max_enroll_stage);

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (device)), 0, 0, NULL);
  g_usb_device_close (usb_dev, NULL);

  fpi_device_probe_complete (device, NULL, product, error);
}

static void
dev_init (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  GError *error = NULL;

  G_DEBUG_HERE ();
  if (!g_usb_device_reset (fpi_device_get_usb_device (device), &error))
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  /* Claim usb interface */
  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device), 0, 0, &error))
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  /* Drain the unsolicited greeting frame the Kensington 047d:8228 pushes
   * onto the bulk-IN endpoint after every USB reset; if left pending it is
   * consumed as the status of the first bulk command and desynchronizes
   * the whole session. Best-effort: absence of data is fine. */
  for (int i = 0; i < 3; i++)
    {
      g_autoptr(FpiUsbTransfer) drain = fpi_usb_transfer_new (device);
      g_autoptr(GError) drain_err = NULL;

      fpi_usb_transfer_fill_bulk (drain, EP_IN, EP_IN_MAX_BUF_SIZE);
      fpi_usb_transfer_submit_sync (drain, 200, &drain_err);
      if (drain_err != NULL)
        break;
      fp_dbg ("drained stale bytes after reset");
    }

  g_assert (!self->task_ssm);

  self->task_ssm = fpi_ssm_new_full (device,
                                     fp_init_sm_run_state,
                                     FP_RTK_INIT_NUM_STATES,
                                     FP_RTK_INIT_NUM_STATES,
                                     "Init");

  fpi_ssm_start (self->task_ssm, fp_init_ssm_done);
}

static void
dev_exit (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);

  g_autoptr(GError) release_error = NULL;

  G_DEBUG_HERE ();

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (self)), 0, 0, &release_error);

  fpi_device_close_complete (device, release_error);
}

static void
delete_print (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);

  G_DEBUG_HERE ();

  g_assert (!self->task_ssm);

  self->task_ssm = fpi_ssm_new_full (device,
                                     fp_delete_sm_run_state,
                                     FP_RTK_DELETE_NUM_STATES,
                                     FP_RTK_DELETE_NUM_STATES,
                                     "Delete print");

  fpi_ssm_start (self->task_ssm, fp_delete_ssm_done);
}

static void
clear_storage (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  G_DEBUG_HERE ();
  co_delete_record.param[0] = 0xff;
  cmd_buf = (guint8 *) &co_delete_record;
  rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 0, fp_clear_storage_cb);
}

static void
list_print (FpDevice *device)
{
  FpiDeviceRealtek *self = FPI_DEVICE_REALTEK (device);
  guint8 *cmd_buf = NULL;

  G_DEBUG_HERE ();

  if (self->template_num < 0)
    {
      fpi_device_list_complete (device, NULL,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "Get template number failed!"));
      return;
    }

  if (self->template_num == 0)
    {
      g_autoptr(GPtrArray) list_result = g_ptr_array_new_with_free_func (g_object_unref);
      fpi_device_list_complete (device, g_steal_pointer (&list_result), NULL);
      return;
    }

  co_get_template.data_len[0] = GET_LEN_L (self->template_len * self->template_num);
  co_get_template.data_len[1] = GET_LEN_H (self->template_len * self->template_num);

  cmd_buf = (guint8 *) &co_get_template;
  rtk_sensor_bulk_cmd (self, cmd_buf, NULL, FP_RTK_MSG_DEFAULT, 1, fp_list_cb);
}

static void
fpi_device_realtek_init (FpiDeviceRealtek *self)
{
}

static void
fpi_device_realtek_class_init (FpiDeviceRealtekClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Realtek MOC Fingerprint Sensor";

  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = MAX_ENROLL_SAMPLES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = dev_init;
  dev_class->close = dev_exit;
  dev_class->probe = dev_probe;
  dev_class->verify = identify_verify;
  dev_class->identify = identify_verify;
  dev_class->enroll = enroll;
  dev_class->delete = delete_print;
  dev_class->clear_storage = clear_storage;
  dev_class->list = list_print;

  fpi_device_class_auto_initialize_features (dev_class);
}
