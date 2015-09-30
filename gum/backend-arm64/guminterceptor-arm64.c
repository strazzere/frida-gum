/*
 * Copyright (C) 2014-2015 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "guminterceptor-priv.h"

#include "gumarm64reader.h"
#include "gumarm64relocator.h"
#include "gumarm64writer.h"
#include "gummemory.h"

#include <string.h>
#include <unistd.h>

#define GUM_ARM64_B_MAX_DISTANCE    0x00fffffff
#define GUM_ARM64_ADRP_MAX_DISTANCE 0x1fffff000

typedef struct _GumFunctionContextBackendData GumFunctionContextBackendData;

struct _GumInterceptorBackend
{
  GumArm64Writer writer;
  GumArm64Relocator relocator;
};

struct _GumFunctionContextBackendData
{
  guint redirect_code_size;
};

static void gum_function_context_clear_cache (GumFunctionContext * ctx);

GumInterceptorBackend *
_gum_interceptor_backend_create (GumCodeAllocator * allocator)
{
  GumInterceptorBackend * backend;

  backend = g_slice_new (GumInterceptorBackend);
  gum_arm64_writer_init (&backend->writer, NULL);
  gum_arm64_relocator_init (&backend->relocator, NULL, &backend->writer);

  return backend;
}

void
_gum_interceptor_backend_destroy (GumInterceptorBackend * backend)
{
  gum_arm64_relocator_free (&backend->relocator);
  gum_arm64_writer_free (&backend->writer);

  g_slice_free (GumInterceptorBackend, backend);
}

static gboolean
gum_interceptor_backend_prepare_trampoline (GumInterceptorBackend * self,
                                            GumFunctionContext * ctx)
{
  GumFunctionContextBackendData * data = (GumFunctionContextBackendData *)
      ctx->backend_data;
  gpointer function_address = ctx->function_address;
  guint redirect_limit;

  if (gum_arm64_relocator_can_relocate (function_address, 16,
      GUM_SCENARIO_ONLINE, &redirect_limit))
  {
    data->redirect_code_size = 16;

    ctx->trampoline_slice = gum_code_allocator_alloc_slice (ctx->allocator);
  }
  else
  {
    GumAddressSpec spec;
    gsize alignment;

    if (redirect_limit >= 8)
    {
      data->redirect_code_size = 8;

      spec.near_address = GSIZE_TO_POINTER (
          GPOINTER_TO_SIZE (function_address) & ~((gsize) (4096 - 1)));
      spec.max_distance = GUM_ARM64_ADRP_MAX_DISTANCE;
      alignment = 4096;
    }
    else if (redirect_limit == 4)
    {
      data->redirect_code_size = 4;

      spec.near_address = function_address;
      spec.max_distance = GUM_ARM64_B_MAX_DISTANCE;
      alignment = 0;
    }
    else
    {
      return FALSE;
    }

    ctx->trampoline_slice = gum_code_allocator_try_alloc_slice_near (
        ctx->allocator, &spec, alignment);
    if (ctx->trampoline_slice == NULL)
      return FALSE;
  }

  return TRUE;
}

gboolean
_gum_interceptor_backend_make_monitor_trampoline (GumInterceptorBackend * self,
                                                  GumFunctionContext * ctx)
{
  GumArm64Writer * aw = &self->writer;
  GumArm64Relocator * ar = &self->relocator;
  gpointer function_address = ctx->function_address;
  GumFunctionContextBackendData * data = (GumFunctionContextBackendData *)
      ctx->backend_data;
  guint reloc_bytes;
  GumAddress resume_at;

  if (!gum_interceptor_backend_prepare_trampoline (self, ctx))
    return FALSE;

  gum_arm64_writer_reset (aw, ctx->trampoline_slice->data);

  /*
   * Generate on_enter trampoline
   */
  ctx->on_enter_trampoline = gum_arm64_writer_cur (aw);

  gum_arm64_writer_put_push_cpu_context (aw);

  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X1,
      ARM64_REG_SP, 8);
  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X2,
      ARM64_REG_X1, G_STRUCT_OFFSET (GumCpuContext, lr));

  gum_arm64_writer_put_call_address_with_arguments (aw,
      GUM_ADDRESS (_gum_function_context_on_enter),
      3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (ctx),
      GUM_ARG_REGISTER, ARM64_REG_X1,
      GUM_ARG_REGISTER, ARM64_REG_X2);

  gum_arm64_writer_put_pop_cpu_context (aw);

  gum_arm64_relocator_reset (ar, function_address, aw);

  do
  {
    reloc_bytes = gum_arm64_relocator_read_one (ar, NULL);
    g_assert_cmpuint (reloc_bytes, !=, 0);
  }
  while (reloc_bytes < data->redirect_code_size);

  gum_arm64_relocator_write_all (ar);

  resume_at = GUM_ADDRESS (function_address) + reloc_bytes;
  gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X16, resume_at);
  gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);

  gum_arm64_writer_flush (aw);
  g_assert_cmpuint (gum_arm64_writer_offset (aw),
      <=, ctx->trampoline_slice->size);

  ctx->overwritten_prologue_len = reloc_bytes;
  memcpy (ctx->overwritten_prologue, function_address, reloc_bytes);

  /*
   * Generate on_leave trampoline
   */
  ctx->on_leave_trampoline = gum_arm64_writer_cur (aw);

  gum_arm64_writer_put_push_cpu_context (aw);

  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X1,
      ARM64_REG_SP, 8);
  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X2,
      ARM64_REG_X1, G_STRUCT_OFFSET (GumCpuContext, lr));

  gum_arm64_writer_put_call_address_with_arguments (aw,
      GUM_ADDRESS (_gum_function_context_on_leave),
      3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (ctx),
      GUM_ARG_REGISTER, ARM64_REG_X1,
      GUM_ARG_REGISTER, ARM64_REG_X2);

  gum_arm64_writer_put_pop_cpu_context (aw);
  gum_arm64_writer_put_br_reg (aw, ARM64_REG_LR);

  gum_arm64_writer_flush (aw);
  g_assert_cmpuint (gum_arm64_writer_offset (aw),
      <=, ctx->trampoline_slice->size);

  return TRUE;
}

gboolean
_gum_interceptor_backend_make_replace_trampoline (GumInterceptorBackend * self,
                                                  GumFunctionContext * ctx,
                                                  gpointer replacement_function)
{
  GumArm64Writer * aw = &self->writer;
  GumArm64Relocator * ar = &self->relocator;
  gpointer function_address = ctx->function_address;
  GumFunctionContextBackendData * data = (GumFunctionContextBackendData *)
      ctx->backend_data;
  gconstpointer skip_label = "gum_interceptor_replacement_skip";
  guint reloc_bytes;
  GumAddress resume_at;

  if (!gum_interceptor_backend_prepare_trampoline (self, ctx))
    return FALSE;

  gum_arm64_writer_reset (aw, ctx->trampoline_slice->data);

  /*
   * Generate on_leave trampoline
   */
  ctx->on_leave_trampoline = gum_arm64_writer_cur (aw);

  gum_arm64_writer_put_push_reg_reg (aw, ARM64_REG_X0, ARM64_REG_X1);
  gum_arm64_writer_put_call_address_with_arguments (aw,
      GUM_ADDRESS (_gum_function_context_end_invocation),
      0);
  gum_arm64_writer_put_mov_reg_reg (aw, ARM64_REG_LR, ARM64_REG_X0);
  gum_arm64_writer_put_pop_reg_reg (aw, ARM64_REG_X0, ARM64_REG_X1);
  gum_arm64_writer_put_br_reg (aw, ARM64_REG_LR);

  /*
   * Generate on_enter trampoline
   */
  ctx->on_enter_trampoline = gum_arm64_writer_cur (aw);

  gum_arm64_writer_put_push_cpu_context (aw);

  gum_arm64_writer_put_add_reg_reg_imm (aw, ARM64_REG_X2,
      ARM64_REG_SP, 8);
  gum_arm64_writer_put_ldr_reg_reg_offset (aw, ARM64_REG_X1,
      ARM64_REG_X2, G_STRUCT_OFFSET (GumCpuContext, lr));

  gum_arm64_writer_put_call_address_with_arguments (aw,
      GUM_ADDRESS (_gum_function_context_try_begin_invocation),
      3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (ctx),
      GUM_ARG_REGISTER, ARM64_REG_X1,
      GUM_ARG_REGISTER, ARM64_REG_X2);
  gum_arm64_writer_put_cbz_reg_label (aw, ARM64_REG_W0, skip_label);

  gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X0,
      GUM_ADDRESS (ctx->on_leave_trampoline));
  gum_arm64_writer_put_str_reg_reg_offset (aw, ARM64_REG_X0,
      ARM64_REG_SP, 8 + G_STRUCT_OFFSET (GumCpuContext, lr));
  gum_arm64_writer_put_pop_cpu_context (aw);
  gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X16,
      GUM_ADDRESS (replacement_function));
  gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);

  gum_arm64_writer_put_label (aw, skip_label);
  gum_arm64_writer_put_pop_cpu_context (aw);

  gum_arm64_relocator_reset (ar, function_address, aw);

  do
  {
    reloc_bytes = gum_arm64_relocator_read_one (ar, NULL);
    g_assert_cmpuint (reloc_bytes, !=, 0);
  }
  while (reloc_bytes < data->redirect_code_size);

  gum_arm64_relocator_write_all (ar);

  resume_at = GUM_ADDRESS (function_address) + reloc_bytes;
  gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X16, resume_at);
  gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);

  gum_arm64_writer_flush (aw);
  g_assert_cmpuint (gum_arm64_writer_offset (aw),
      <=, ctx->trampoline_slice->size);

  ctx->overwritten_prologue_len = reloc_bytes;
  memcpy (ctx->overwritten_prologue, function_address, reloc_bytes);

  return TRUE;
}

void
_gum_interceptor_backend_destroy_trampoline (GumInterceptorBackend * self,
                                             GumFunctionContext * ctx)
{
  gum_code_allocator_free_slice (ctx->allocator, ctx->trampoline_slice);
  ctx->trampoline_slice = NULL;
}

void
_gum_interceptor_backend_activate_trampoline (GumInterceptorBackend * self,
                                              GumFunctionContext * ctx)
{
  GumArm64Writer * aw = &self->writer;
  GumFunctionContextBackendData * data = (GumFunctionContextBackendData *)
      ctx->backend_data;
  GumAddress on_enter = GUM_ADDRESS (ctx->on_enter_trampoline);

  gum_arm64_writer_reset (aw, ctx->function_address);
  switch (data->redirect_code_size)
  {
    case 4:
      gum_arm64_writer_put_b_imm (aw, on_enter);
      break;
    case 8:
      gum_arm64_writer_put_adrp_reg_address (aw, ARM64_REG_X16, on_enter);
      gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);
      break;
    case 16:
      gum_arm64_writer_put_ldr_reg_address (aw, ARM64_REG_X16, on_enter);
      gum_arm64_writer_put_br_reg (aw, ARM64_REG_X16);
      break;
    default:
      g_assert_not_reached ();
  }
  gum_arm64_writer_flush (aw);

  gum_function_context_clear_cache (ctx);
}

void
_gum_interceptor_backend_deactivate_trampoline (GumInterceptorBackend * self,
                                                GumFunctionContext * ctx)
{
  memcpy (ctx->function_address, ctx->overwritten_prologue,
      ctx->overwritten_prologue_len);
  gum_function_context_clear_cache (ctx);
}

static void
gum_function_context_clear_cache (GumFunctionContext * ctx)
{
  gum_clear_cache (ctx->function_address, ctx->overwritten_prologue_len);
  gum_clear_cache (ctx->trampoline_slice->data, ctx->trampoline_slice->size);
}

gpointer
_gum_interceptor_backend_resolve_redirect (GumInterceptorBackend * self,
                                           gpointer address)
{
  return gum_arm64_reader_try_get_relative_jump_target (address);
}

gboolean
_gum_interceptor_backend_can_intercept (GumInterceptorBackend * self,
                                        gpointer function_address)
{
  return TRUE;
}

gpointer
_gum_interceptor_invocation_get_nth_argument (GumInvocationContext * context,
                                              guint n)
{
  if (n < 8)
  {
    return (gpointer) context->cpu_context->x[n];
  }
  else
  {
    gpointer * stack_argument = (gpointer *) context->cpu_context->sp;

    return stack_argument[n - 8];
  }
}

void
_gum_interceptor_invocation_replace_nth_argument (
    GumInvocationContext * context,
    guint n,
    gpointer value)
{
  if (n < 8)
  {
    context->cpu_context->x[n] = (guint64) value;
  }
  else
  {
    gpointer * stack_argument = (gpointer *) context->cpu_context->sp;

    stack_argument[n - 8] = value;
  }
}

gpointer
_gum_interceptor_invocation_get_return_value (GumInvocationContext * context)
{
  return (gpointer) context->cpu_context->x[0];
}

void
_gum_interceptor_invocation_replace_return_value (
    GumInvocationContext * context,
    gpointer value)
{
  context->cpu_context->x[0] = (guint64) value;
}

