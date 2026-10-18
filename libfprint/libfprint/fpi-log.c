/*
 * FpiLog - Internal logging functions
 * Copyright (C) 2020 Benjamin Berg <bberg@redhat.com>
 * Copyright (C) 2025 Joshua Grisham <josh@joshuagrisham.com>
 * Copyright (C) 2026 Marco Trevisan (Treviño) <mail@3v1n0.net>
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

#include "fpi-log.h"

/**
 * fpi_log_is_debug_transfer_enabled:
 *
 * Checks if the %FP_DEBUG_TRANSFER environment variable is set.
 *
 * Returns: %TRUE if %FP_DEBUG_TRANSFER is set, %FALSE otherwise
 */
gboolean
fpi_log_is_debug_transfer_enabled (void)
{
  static gsize debug_transfer_enabled = 0;

  if (g_once_init_enter (&debug_transfer_enabled))
    {
      gsize enabled = g_getenv ("FP_DEBUG_TRANSFER") != NULL ? TRUE : G_MAXSIZE;
      g_once_init_leave (&debug_transfer_enabled, enabled);
    }

  return debug_transfer_enabled == TRUE;
}

/**
 * fpi_dbg_hex_dump_data:
 * @buf: Bytes buffer to dump
 * @len: Length of @buf to dump
 *
 * Prints hex dump of @buf to fp_dbg()
 */
void
  (fpi_dbg_hex_dump_data) (const gchar  *log_domain,
                           const guint8 *buf,
                           gsize len)
{
  g_autoptr(GString) line = NULL;

  if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, log_domain))
    return;

  if (G_UNLIKELY (len == 0 || !buf))
    return;

  line = g_string_new ("");

  for (gint i = 0; i < len; i++)
    g_string_append_printf (line, "%02x", buf[i]);

  if (line->len)
    g_log (log_domain, G_LOG_LEVEL_DEBUG, "%s", line->str);
}

/**
 * fpi_dbg_hex_dump_bytes:
 * @bytes: #GBytes to dump
 *
 * Prints hex dump of @bytes to fp_dbg()
 */
void
  (fpi_dbg_hex_dump_bytes) (const gchar *log_domain,
                            GBytes      *bytes)
{
  gsize length = 0;
  const guint8 *data = g_bytes_get_data (bytes, &length);

  (fpi_dbg_hex_dump_data) (log_domain, data, length);
}
