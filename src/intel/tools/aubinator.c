/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "util/macros.h"

#include "common/gen_decoder.h"
#include "aub_read.h"
#include "aub_mem.h"

#define CSI "\e["
#define BLUE_HEADER  CSI "0;44m"
#define GREEN_HEADER CSI "1;42m"
#define NORMAL       CSI "0m"

/* options */

static int option_full_decode = true;
static int option_print_offsets = true;
static int max_vbo_lines = -1;
static enum { COLOR_AUTO, COLOR_ALWAYS, COLOR_NEVER } option_color;

/* state */

uint16_t pci_id = 0;
char *input_file = NULL, *xml_path = NULL;
struct gen_device_info devinfo;
struct gen_batch_decode_ctx batch_ctx;
struct aub_mem mem;

FILE *outfile;

struct brw_instruction;

static void
aubinator_error(void *user_data, const void *aub_data, const char *msg)
{
   fprintf(stderr, "%s", msg);
}

static void
aubinator_init(void *user_data, int aub_pci_id, const char *app_name)
{
   pci_id = aub_pci_id;

   if (!gen_get_device_info(pci_id, &devinfo)) {
      fprintf(stderr, "can't find device information: pci_id=0x%x\n", pci_id);
      exit(EXIT_FAILURE);
   }

   enum gen_batch_decode_flags batch_flags = 0;
   if (option_color == COLOR_ALWAYS)
      batch_flags |= GEN_BATCH_DECODE_IN_COLOR;
   if (option_full_decode)
      batch_flags |= GEN_BATCH_DECODE_FULL;
   if (option_print_offsets)
      batch_flags |= GEN_BATCH_DECODE_OFFSETS;
   batch_flags |= GEN_BATCH_DECODE_FLOATS;

   gen_batch_decode_ctx_init(&batch_ctx, &devinfo, outfile, batch_flags,
                             xml_path, NULL, NULL, NULL);

   /* Check for valid spec instance, if wrong xml_path is passed then spec
    * instance is not initialized properly
    */
   if (!batch_ctx.spec) {
      fprintf(stderr, "Failed to initialize gen_batch_decode_ctx "
                      "spec instance\n");
      free(xml_path);
      gen_batch_decode_ctx_finish(&batch_ctx);
      exit(EXIT_FAILURE);
   }

   batch_ctx.max_vbo_decoded_lines = max_vbo_lines;

   char *color = GREEN_HEADER, *reset_color = NORMAL;
   if (option_color == COLOR_NEVER)
      color = reset_color = "";

   fprintf(outfile, "%sAubinator: Intel AUB file decoder.%-80s%s\n",
           color, "", reset_color);

   if (input_file)
      fprintf(outfile, "File name:        %s\n", input_file);

   if (aub_pci_id)
      fprintf(outfile, "PCI ID:           0x%x\n", aub_pci_id);

   fprintf(outfile, "Application name: %s\n", app_name);

   fprintf(outfile, "Decoding as:      %s\n", gen_get_device_name(pci_id));

   /* Throw in a new line before the first batch */
   fprintf(outfile, "\n");
}

static void
handle_execlist_write(void *user_data, enum gen_engine engine, uint64_t context_descriptor)
{
   const uint32_t pphwsp_size = 4096;
   uint32_t pphwsp_addr = context_descriptor & 0xfffff000;
   struct gen_batch_decode_bo pphwsp_bo = aub_mem_get_ggtt_bo(&mem, pphwsp_addr);
   uint32_t *context = (uint32_t *)((uint8_t *)pphwsp_bo.map +
                                    (pphwsp_addr - pphwsp_bo.addr) +
                                    pphwsp_size);

   uint32_t ring_buffer_head = context[5];
   uint32_t ring_buffer_tail = context[7];
   uint32_t ring_buffer_start = context[9];

   mem.pml4 = (uint64_t)context[49] << 32 | context[51];
   batch_ctx.user_data = &mem;

   struct gen_batch_decode_bo ring_bo = aub_mem_get_ggtt_bo(&mem,
                                                            ring_buffer_start);
   assert(ring_bo.size > 0);
   void *commands = (uint8_t *)ring_bo.map + (ring_buffer_start - ring_bo.addr);

   if (context_descriptor & 0x100 /* ppgtt */) {
      batch_ctx.get_bo = aub_mem_get_ppgtt_bo;
   } else {
      batch_ctx.get_bo = aub_mem_get_ggtt_bo;
   }

   (void)engine; /* TODO */
   gen_print_batch(&batch_ctx, commands, ring_buffer_tail - ring_buffer_head,
                   0);
   aub_mem_clear_bo_maps(&mem);
}

static void
handle_ring_write(void *user_data, enum gen_engine engine,
                  const void *data, uint32_t data_len)
{
   batch_ctx.user_data = &mem;
   batch_ctx.get_bo = aub_mem_get_ggtt_bo;

   gen_print_batch(&batch_ctx, data, data_len, 0);

   aub_mem_clear_bo_maps(&mem);
}

struct aub_file {
   FILE *stream;

   void *map, *end, *cursor;
};

static struct aub_file *
aub_file_open(const char *filename)
{
   struct aub_file *file;
   struct stat sb;
   int fd;

   file = calloc(1, sizeof *file);
   if (file == NULL)
      return NULL;

   fd = open(filename, O_RDONLY);
   if (fd == -1) {
      fprintf(stderr, "open %s failed: %s\n", filename, strerror(errno));
      free(file);
      exit(EXIT_FAILURE);
   }

   if (fstat(fd, &sb) == -1) {
      fprintf(stderr, "stat failed: %s\n", strerror(errno));
      free(file);
      exit(EXIT_FAILURE);
   }

   file->map = mmap(NULL, sb.st_size,
                    PROT_READ, MAP_SHARED, fd, 0);
   if (file->map == MAP_FAILED) {
      fprintf(stderr, "mmap failed: %s\n", strerror(errno));
      free(file);
      exit(EXIT_FAILURE);
   }

   close(fd);

   file->cursor = file->map;
   file->end = file->map + sb.st_size;

   return file;
}

static int
aub_file_more_stuff(struct aub_file *file)
{
   return file->cursor < file->end || (file->stream && !feof(file->stream));
}

static void
setup_pager(void)
{
   int fds[2];
   pid_t pid;

   if (!isatty(1))
      return;

   if (pipe(fds) == -1)
      return;

   pid = fork();
   if (pid == -1)
      return;

   if (pid == 0) {
      close(fds[1]);
      dup2(fds[0], 0);
      execlp("less", "less", "-FRSi", NULL);
   }

   close(fds[0]);
   dup2(fds[1], 1);
   close(fds[1]);
}

static void
print_help(const char *progname, FILE *file)
{
   fprintf(file,
           "Usage: %s [OPTION]... FILE\n"
           "Decode aub file contents from FILE.\n\n"
           "      --help             display this help and exit\n"
           "      --gen=platform     decode for given platform (3 letter platform name)\n"
           "      --headers          decode only command headers\n"
           "      --color[=WHEN]     colorize the output; WHEN can be 'auto' (default\n"
           "                         if omitted), 'always', or 'never'\n"
           "      --max-vbo-lines=N  limit the number of decoded VBO lines\n"
           "      --no-pager         don't launch pager\n"
           "      --no-offsets       don't print instruction offsets\n"
           "      --xml=DIR          load hardware xml description from directory DIR\n",
           progname);
}

int main(int argc, char *argv[])
{
   struct aub_file *file;
   int c, i;
   bool help = false, pager = true;
   const struct option aubinator_opts[] = {
      { "help",          no_argument,       (int *) &help,                 true },
      { "no-pager",      no_argument,       (int *) &pager,                false },
      { "no-offsets",    no_argument,       (int *) &option_print_offsets, false },
      { "gen",           required_argument, NULL,                          'g' },
      { "headers",       no_argument,       (int *) &option_full_decode,   false },
      { "color",         required_argument, NULL,                          'c' },
      { "xml",           required_argument, NULL,                          'x' },
      { "max-vbo-lines", required_argument, NULL,                          'v' },
      { NULL,            0,                 NULL,                          0 }
   };

   outfile = stdout;

   i = 0;
   while ((c = getopt_long(argc, argv, "", aubinator_opts, &i)) != -1) {
      switch (c) {
      case 'g': {
         const int id = gen_device_name_to_pci_device_id(optarg);
         if (id < 0) {
            fprintf(stderr, "can't parse gen: '%s', expected brw, g4x, ilk, "
                            "snb, ivb, hsw, byt, bdw, chv, skl, bxt, kbl, "
                            "aml, glk, cfl, cnl, icl", optarg);
            exit(EXIT_FAILURE);
         } else {
            pci_id = id;
         }
         break;
      }
      case 'c':
         if (optarg == NULL || strcmp(optarg, "always") == 0)
            option_color = COLOR_ALWAYS;
         else if (strcmp(optarg, "never") == 0)
            option_color = COLOR_NEVER;
         else if (strcmp(optarg, "auto") == 0)
            option_color = COLOR_AUTO;
         else {
            fprintf(stderr, "invalid value for --color: %s", optarg);
            exit(EXIT_FAILURE);
         }
         break;
      case 'x':
         xml_path = strdup(optarg);
         break;
      case 'v':
         max_vbo_lines = atoi(optarg);
         break;
      default:
         break;
      }
   }

   if (optind < argc)
      input_file = argv[optind];

   if (help || !input_file) {
      print_help(argv[0], stderr);
      exit(0);
   }

   /* Do this before we redirect stdout to pager. */
   if (option_color == COLOR_AUTO)
      option_color = isatty(1) ? COLOR_ALWAYS : COLOR_NEVER;

   if (isatty(1) && pager)
      setup_pager();

   if (!aub_mem_init(&mem)) {
      fprintf(stderr, "Unable to create GTT\n");
      exit(EXIT_FAILURE);
   }

   file = aub_file_open(input_file);
   if (!file) {
      fprintf(stderr, "Unable to allocate buffer to open aub file\n");
      free(xml_path);
      exit(EXIT_FAILURE);
   }

   struct aub_read aub_read = {
      .user_data = &mem,
      .error = aubinator_error,
      .info = aubinator_init,

      .local_write = aub_mem_local_write,
      .phys_write = aub_mem_phys_write,
      .ggtt_write = aub_mem_ggtt_write,
      .ggtt_entry_write = aub_mem_ggtt_entry_write,

      .execlist_write = handle_execlist_write,
      .ring_write = handle_ring_write,
   };
   int consumed;
   while (aub_file_more_stuff(file) &&
          (consumed = aub_read_command(&aub_read, file->cursor,
                                       file->end - file->cursor)) > 0) {
      file->cursor += consumed;
   }

   aub_mem_fini(&mem);

   fflush(stdout);
   /* close the stdout which is opened to write the output */
   close(1);
   free(file);
   free(xml_path);

   wait(NULL);
   gen_batch_decode_ctx_finish(&batch_ctx);

   return EXIT_SUCCESS;
}
