/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2020  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  UEFI Secure Boot related checkings.
 */

#include <grub/efi/efi.h>
#include <grub/efi/pe32.h>
#include <grub/efi/sb.h>
#include <grub/env.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/i386/linux.h>
#include <grub/kernel.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/verify.h>

static grub_guid_t shim_lock_guid = GRUB_EFI_SHIM_LOCK_GUID;
static grub_guid_t shim_loader_guid = GRUB_EFI_SHIM_IMAGE_LOADER_GUID;

static grub_efi_loader_t *shim_loader = NULL;

/*
 * Determine whether we're in secure boot mode.
 *
 * Please keep the logic in sync with the Linux kernel,
 * drivers/firmware/efi/libstub/secureboot.c:efi_get_secureboot().
 */
grub_uint8_t
grub_efi_get_secureboot (void)
{
  static grub_guid_t efi_variable_guid = GRUB_EFI_GLOBAL_VARIABLE_GUID;
  grub_efi_status_t status;
  grub_efi_uint32_t attr = 0;
  grub_size_t size = 0;
  grub_uint8_t *secboot = NULL;
  grub_uint8_t *setupmode = NULL;
  grub_uint8_t *moksbstate = NULL;
  grub_uint8_t secureboot = GRUB_EFI_SECUREBOOT_MODE_UNKNOWN;
  const char *secureboot_str = "UNKNOWN";

  status = grub_efi_get_variable ("SecureBoot", &efi_variable_guid,
				  &size, (void **) &secboot);

  if (status == GRUB_EFI_NOT_FOUND)
    {
      secureboot = GRUB_EFI_SECUREBOOT_MODE_DISABLED;
      goto out;
    }

  if (status != GRUB_EFI_SUCCESS)
    goto out;

  status = grub_efi_get_variable ("SetupMode", &efi_variable_guid,
				  &size, (void **) &setupmode);

  if (status != GRUB_EFI_SUCCESS)
    goto out;

  if ((*secboot == 0) || (*setupmode == 1))
    {
      secureboot = GRUB_EFI_SECUREBOOT_MODE_DISABLED;
      goto out;
    }

  /*
   * See if a user has put the shim into insecure mode. If so, and if the
   * variable doesn't have the runtime attribute set, we might as well
   * honor that.
   */
  status = grub_efi_get_variable_with_attributes ("MokSBState", &shim_lock_guid,
						  &size, (void **) &moksbstate, &attr);

  /* If it fails, we don't care why. Default to secure. */
  if (status != GRUB_EFI_SUCCESS)
    {
      secureboot = GRUB_EFI_SECUREBOOT_MODE_ENABLED;
      goto out;
    }

  if (!(attr & GRUB_EFI_VARIABLE_RUNTIME_ACCESS) && *moksbstate == 1)
    {
      secureboot = GRUB_EFI_SECUREBOOT_MODE_DISABLED;
      goto out;
    }

  secureboot = GRUB_EFI_SECUREBOOT_MODE_ENABLED;

 out:
  grub_free (moksbstate);
  grub_free (setupmode);
  grub_free (secboot);

  if (secureboot == GRUB_EFI_SECUREBOOT_MODE_DISABLED)
    secureboot_str = "Disabled";
  else if (secureboot == GRUB_EFI_SECUREBOOT_MODE_ENABLED)
    secureboot_str = "Enabled";

  grub_dprintf ("efi", "UEFI Secure Boot state: %s\n", secureboot_str);

  return secureboot;
}

static grub_err_t
shim_lock_verifier_init (grub_file_t io __attribute__ ((unused)),
			 enum grub_file_type type,
			 void **context __attribute__ ((unused)),
			 enum grub_verify_flags *flags)
{
  *flags = GRUB_VERIFY_FLAGS_NONE;

  switch (type & GRUB_FILE_TYPE_MASK)
    {
    /* Files we check. */
    case GRUB_FILE_TYPE_LINUX_KERNEL:
    case GRUB_FILE_TYPE_MULTIBOOT_KERNEL:
    case GRUB_FILE_TYPE_BSD_KERNEL:
    case GRUB_FILE_TYPE_XNU_KERNEL:
    case GRUB_FILE_TYPE_PLAN9_KERNEL:
    case GRUB_FILE_TYPE_EFI_CHAINLOADED_IMAGE:
      *flags = GRUB_VERIFY_FLAGS_SINGLE_CHUNK;
      return GRUB_ERR_NONE;

    /* Files that do not affect secureboot state. */
    case GRUB_FILE_TYPE_NONE:
    case GRUB_FILE_TYPE_LOOPBACK:
    case GRUB_FILE_TYPE_LINUX_INITRD:
    case GRUB_FILE_TYPE_OPENBSD_RAMDISK:
    case GRUB_FILE_TYPE_XNU_RAMDISK:
    case GRUB_FILE_TYPE_SIGNATURE:
    case GRUB_FILE_TYPE_PUBLIC_KEY:
    case GRUB_FILE_TYPE_PUBLIC_KEY_TRUST:
    case GRUB_FILE_TYPE_PRINT_BLOCKLIST:
    case GRUB_FILE_TYPE_TESTLOAD:
    case GRUB_FILE_TYPE_GET_SIZE:
    case GRUB_FILE_TYPE_ZFS_ENCRYPTION_KEY:
    case GRUB_FILE_TYPE_CAT:
    case GRUB_FILE_TYPE_HEXCAT:
    case GRUB_FILE_TYPE_CMP:
    case GRUB_FILE_TYPE_HASHLIST:
    case GRUB_FILE_TYPE_TO_HASH:
    case GRUB_FILE_TYPE_KEYBOARD_LAYOUT:
    case GRUB_FILE_TYPE_PIXMAP:
    case GRUB_FILE_TYPE_GRUB_MODULE_LIST:
    case GRUB_FILE_TYPE_CONFIG:
    case GRUB_FILE_TYPE_THEME:
    case GRUB_FILE_TYPE_GETTEXT_CATALOG:
    case GRUB_FILE_TYPE_FS_SEARCH:
    case GRUB_FILE_TYPE_LOADENV:
    case GRUB_FILE_TYPE_SAVEENV:
    case GRUB_FILE_TYPE_VERIFY_SIGNATURE:
      *flags = GRUB_VERIFY_FLAGS_SKIP_VERIFICATION;
      return GRUB_ERR_NONE;

    /* Other files. */
    default:
      return grub_error (GRUB_ERR_ACCESS_DENIED, N_("prohibited by secure boot policy"));
    }
}

static grub_err_t
shim_lock_verifier_write (void *context __attribute__ ((unused)), void *buf, grub_size_t size)
{
  grub_efi_handle_t image_handle;

  if (!shim_loader)
    return grub_error (GRUB_ERR_ACCESS_DENIED, N_("shim loader protocol not found"));

  if (shim_loader->load_image (false, grub_efi_image_handle, NULL, buf, size, &image_handle) != GRUB_EFI_SUCCESS)
    return grub_error (GRUB_ERR_BAD_SIGNATURE, N_("bad shim signature"));

  shim_loader->unload_image(image_handle);

  return GRUB_ERR_NONE;
}

struct grub_file_verifier shim_lock_verifier =
  {
    .name = "shim_lock_verifier",
    .init = shim_lock_verifier_init,
    .write = shim_lock_verifier_write
  };

void
grub_shim_lock_verifier_setup (void)
{
  struct grub_module_header *header;
  shim_loader = grub_efi_locate_protocol (&shim_loader_guid, 0);

  /* shim loader protocol is missing, check if GRUB image is built with --disable-shim-lock. */
  if (!shim_loader)
    {
      FOR_MODULES (header)
	{
	  if (header->type == OBJ_TYPE_DISABLE_SHIM_LOCK)
	    return;
	}
    }

  /* Secure Boot is off. Do not load shim_lock. */
  if (grub_efi_get_secureboot () != GRUB_EFI_SECUREBOOT_MODE_ENABLED)
    return;

  /* register loader */
  grub_efi_register_loader(shim_loader);

  /* Enforce shim_lock_verifier. */
  grub_verifier_register (&shim_lock_verifier);

  grub_env_set ("shim_lock", "y");
  grub_env_export ("shim_lock");
}

int
grub_efi_check_nx_required (void)
{
  int nx_required = 1; /* assume required, unless we can prove otherwise */
  grub_efi_status_t status;
  grub_size_t mok_policy_sz = 0;
  char *mok_policy = NULL;
  grub_uint32_t mok_policy_attrs = 0;

  status = grub_efi_get_variable_with_attributes ("MokPolicy",
						  &(grub_guid_t) GRUB_EFI_SHIM_LOCK_GUID,
						  &mok_policy_sz,
						  (void **)&mok_policy,
						  &mok_policy_attrs);
  if (status != GRUB_EFI_SUCCESS ||
      mok_policy_sz != 1 ||
      mok_policy == NULL ||
      mok_policy_attrs != GRUB_EFI_VARIABLE_BOOTSERVICE_ACCESS)
    goto out;

  nx_required = !!(mok_policy[0] & GRUB_MOK_POLICY_NX_REQUIRED);

 out:
  grub_free (mok_policy);

  return nx_required;
}
