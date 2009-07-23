/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * upbc is the upb compiler.  This is some deep code that I wish could be
 * easier to understand, but by its nature it is doing some very "meta"
 * kinds of things.
 *
 * TODO: compiler currently has memory leaks (trivial to fix with valgrind).
 *
 * Copyright (c) 2009 Joshua Haberman.  See LICENSE for details.
 */

#include <ctype.h>
#include <inttypes.h>
#include "descriptor.h"
#include "upb_context.h"
#include "upb_enum.h"
#include "upb_msg.h"
#include "upb_text.h"

/* These are in-place string transformations that do not change the length of
 * the string (and thus never need to re-allocate). */
static void to_cident(struct upb_string str)
{
  for(uint32_t i = 0; i < str.byte_len; i++)
    if(str.ptr[i] == '.' || str.ptr[i] == '/')
      str.ptr[i] = '_';
}

static void to_preproc(struct upb_string str)
{
  to_cident(str);
  for(uint32_t i = 0; i < str.byte_len; i++)
    str.ptr[i] = toupper(str.ptr[i]);
}

static int memrchr(char *data, char c, size_t len)
{
  int off = len-1;
  while(off > 0 && data[off] != c) --off;
  return off;
}

void *strtable_to_array(struct upb_strtable *t, int *size)
{
  *size = t->t.count;
  void **array = malloc(*size * sizeof(void*));
  struct upb_symtab_entry *e;
  int i = 0;
  for(e = upb_strtable_begin(t); e && i < *size; e = upb_strtable_next(t, &e->e))
    array[i++] = e;
  assert(i == *size && e == NULL);
  return array;
}

/* The .h file defines structs for the types defined in the .proto file.  It
 * also defines constants for the enum values.
 *
 * Assumes that d has been validated. */
static void write_h(struct upb_symtab_entry *entries[], int num_entries,
                    char *outfile_name, FILE *stream)
{
  /* Header file prologue. */
  struct upb_string include_guard_name = upb_strdupc(outfile_name);
  to_preproc(include_guard_name);
  fputs("/* This file was generated by upbc (the upb compiler).  "
        "Do not edit. */\n\n", stream),
  fprintf(stream, "#ifndef " UPB_STRFMT "\n", UPB_STRARG(include_guard_name));
  fprintf(stream, "#define " UPB_STRFMT "\n\n", UPB_STRARG(include_guard_name));
  fputs("#include <upb_string.h>\n\n", stream);
  fputs("#include <upb_array.h>\n\n", stream);
  fputs("#ifdef __cplusplus\n", stream);
  fputs("extern \"C\" {\n", stream);
  fputs("#endif\n\n", stream);

  /* Enums. */
  fprintf(stream, "/* Enums. */\n\n");
  for(int i = 0; i < num_entries; i++) {  /* Foreach enum */
    if(entries[i]->type != UPB_SYM_ENUM) continue;
    struct upb_symtab_entry *entry = entries[i];
    struct upb_enum *e = entry->ref._enum;
    google_protobuf_EnumDescriptorProto *ed = e->descriptor;
    /* We use entry->e.key (the fully qualified name) instead of ed->name. */
    struct upb_string enum_name = upb_strdup(entry->e.key);
    to_cident(enum_name);

    struct upb_string enum_val_prefix = upb_strdup(entry->e.key);
    enum_val_prefix.byte_len = memrchr(enum_val_prefix.ptr,
                                       UPB_SYMBOL_SEPARATOR,
                                       enum_val_prefix.byte_len);
    enum_val_prefix.byte_len++;
    to_preproc(enum_val_prefix);

    fprintf(stream, "typedef enum " UPB_STRFMT " {\n", UPB_STRARG(enum_name));
    if(ed->set_flags.has.value) {
      for(uint32_t j = 0; j < ed->value->len; j++) {  /* Foreach enum value. */
        google_protobuf_EnumValueDescriptorProto *v = ed->value->elements[j];
        struct upb_string value_name = upb_strdup(*v->name);
        to_preproc(value_name);
        /* "  GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_UINT32 = 13," */
        fprintf(stream, "  " UPB_STRFMT UPB_STRFMT " = %" PRIu32,
                UPB_STRARG(enum_val_prefix), UPB_STRARG(value_name), v->number);
        if(j != ed->value->len-1) fputc(',', stream);
        fputc('\n', stream);
        upb_strfree(value_name);
      }
    }
    fprintf(stream, "} " UPB_STRFMT ";\n\n", UPB_STRARG(enum_name));
    upb_strfree(enum_name);
    upb_strfree(enum_val_prefix);
  }

  /* Forward declarations. */
  fputs("/* Forward declarations of all message types.\n", stream);
  fputs(" * So they can refer to each other in ", stream);
  fputs("possibly-recursive ways. */\n\n", stream);

  for(int i = 0; i < num_entries; i++) {  /* Foreach message */
    if(entries[i]->type != UPB_SYM_MESSAGE) continue;
    struct upb_symtab_entry *entry = entries[i];
    /* We use entry->e.key (the fully qualified name). */
    struct upb_string msg_name = upb_strdup(entry->e.key);
    to_cident(msg_name);
    fprintf(stream, "struct " UPB_STRFMT ";\n", UPB_STRARG(msg_name));
    fprintf(stream, "typedef struct " UPB_STRFMT "\n    " UPB_STRFMT ";\n\n",
            UPB_STRARG(msg_name), UPB_STRARG(msg_name));
    upb_strfree(msg_name);
  }

  /* Message Declarations. */
  fputs("/* The message definitions themselves. */\n\n", stream);
  for(int i = 0; i < num_entries; i++) {  /* Foreach message */
    if(entries[i]->type != UPB_SYM_MESSAGE) continue;
    struct upb_symtab_entry *entry = entries[i];
    struct upb_msg *m = entry->ref.msg;
    /* We use entry->e.key (the fully qualified name). */
    struct upb_string msg_name = upb_strdup(entry->e.key);
    to_cident(msg_name);
    fprintf(stream, "struct " UPB_STRFMT " {\n", UPB_STRARG(msg_name));
    fputs("  union {\n", stream);
    fprintf(stream, "    uint8_t bytes[%" PRIu32 "];\n", m->set_flags_bytes);
    fputs("    struct {\n", stream);
    for(uint32_t j = 0; j < m->num_fields; j++) {
      static char* labels[] = {"", "optional", "required", "repeated"};
      struct google_protobuf_FieldDescriptorProto *fd = m->field_descriptors[j];
      fprintf(stream, "      bool " UPB_STRFMT ":1;  /* = %" PRIu32 ", %s. */\n",
              UPB_STRARG(*fd->name), fd->number, labels[fd->label]);
    }
    fputs("    } has;\n", stream);
    fputs("  } set_flags;\n", stream);
    for(uint32_t j = 0; j < m->num_fields; j++) {
      struct upb_msg_field *f = &m->fields[j];
      struct google_protobuf_FieldDescriptorProto *fd = m->field_descriptors[j];
      if(f->type == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_GROUP ||
         f->type == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_MESSAGE) {
        /* Submessages get special treatment, since we have to use the message
         * name directly. */
        struct upb_string type_name_ref = *fd->type_name;
        if(type_name_ref.ptr[0] == UPB_SYMBOL_SEPARATOR) {
          /* Omit leading '.'. */
          type_name_ref.ptr++;
          type_name_ref.byte_len--;
        }
        struct upb_string type_name = upb_strdup(type_name_ref);
        to_cident(type_name);
        if(f->label == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_LABEL_REPEATED) {
          fprintf(stream, "  UPB_MSG_ARRAY(" UPB_STRFMT ")* " UPB_STRFMT ";\n",
                  UPB_STRARG(type_name), UPB_STRARG(*fd->name));
        } else {
          fprintf(stream, "  " UPB_STRFMT "* " UPB_STRFMT ";\n",
                  UPB_STRARG(type_name), UPB_STRARG(*fd->name));
        }
        upb_strfree(type_name);
      } else if(f->label == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_LABEL_REPEATED) {
        static char* c_types[] = {
          "", "struct upb_double_array*", "struct upb_float_array*",
          "struct upb_int64_array*", "struct upb_uint64_array*",
          "struct upb_int32_array*", "struct upb_uint64_array*",
          "struct upb_uint32_array*", "struct upb_bool_array*",
          "struct upb_string_array*", "", "",
          "struct upb_string_array*", "struct upb_uint32_array*",
          "struct upb_uint32_array*", "struct upb_int32_array*",
          "struct upb_int64_array*", "struct upb_int32_array*",
          "struct upb_int64_array*"
        };
        fprintf(stream, "  %s " UPB_STRFMT ";\n",
                c_types[fd->type], UPB_STRARG(*fd->name));
      } else {
        static char* c_types[] = {
          "", "double", "float", "int64_t", "uint64_t", "int32_t", "uint64_t",
          "uint32_t", "bool", "struct upb_string*", "", "",
          "struct upb_string*", "uint32_t", "uint32_t", "int32_t", "int64_t",
          "int32_t", "int64_t"
        };
        fprintf(stream, "  %s " UPB_STRFMT ";\n",
                c_types[fd->type], UPB_STRARG(*fd->name));
      }
    }
    fputs("};\n", stream);
    fprintf(stream, "UPB_DEFINE_MSG_ARRAY(" UPB_STRFMT ")\n\n",
            UPB_STRARG(msg_name));
    upb_strfree(msg_name);
  }

  /* Epilogue. */
  fputs("#ifdef __cplusplus\n", stream);
  fputs("}  /* extern \"C\" */\n", stream);
  fputs("#endif\n\n", stream);
  fprintf(stream, "#endif  /* " UPB_STRFMT " */\n", UPB_STRARG(include_guard_name));
  upb_strfree(include_guard_name);
}

/* Format of table entries that we use when analyzing data structures for
 * write_messages_c. */
struct strtable_entry {
  struct upb_strtable_entry e;
  int offset;
  int num;
};

struct typetable_entry {
  struct upb_strtable_entry e;
  struct upb_msg_field *field;
  struct upb_string c_ident;  /* Type name converted with to_cident(). */
  /* A list of all values of this type, in an established order. */
  union upb_value *values;
  int values_size, values_len;
  struct array {
    int offset;
    int len;
    struct upb_array *ptr;  /* So we can find it later. */
  } *arrays;
  int arrays_size, arrays_len;
};

struct msgtable_entry {
  struct upb_inttable_entry e;
  void *msg;
  int num;  /* Unique offset into the list of all msgs of this type. */
};

int compare_entries(const void *_e1, const void *_e2)
{
  struct strtable_entry *const*e1 = _e1, *const*e2 = _e2;
  return upb_strcmp((*e1)->e.key, (*e2)->e.key);
}

/* Mutually recursive functions to recurse over a set of possibly nested
 * messages and extract all the strings.
 *
 * TODO: make these use a generic msg visitor. */

static void add_strings_from_msg(void *data, struct upb_msg *m,
                                 struct upb_strtable *t);

static void add_strings_from_value(union upb_value_ptr p,
                                   struct upb_msg_field *f,
                                   struct upb_strtable *t)
{
  if(upb_isstringtype(f->type)) {
    struct strtable_entry e = {.e = {.key = **p.str}};
    if(upb_strtable_lookup(t, &e.e.key) == NULL)
      upb_strtable_insert(t, &e.e);
  } else if(upb_issubmsg(f)) {
    add_strings_from_msg(*p.msg, f->ref.msg, t);
  }
}

static void add_strings_from_msg(void *data, struct upb_msg *m,
                                 struct upb_strtable *t)
{
  for(uint32_t i = 0; i < m->num_fields; i++) {
    struct upb_msg_field *f = &m->fields[i];
    if(!upb_msg_isset(data, f)) continue;
    union upb_value_ptr p = upb_msg_getptr(data, f);
    if(upb_isarray(f)) {
      struct upb_array *arr = *p.arr;
      for(uint32_t j = 0; j < arr->len; j++)
        add_strings_from_value(upb_array_getelementptr(arr, j, f->type), f, t);
    } else {
      add_strings_from_value(p, f, t);
    }
  }
}

/* Mutually recursive functions to recurse over a set of possibly nested
 * messages and extract all the messages (keyed by type).
 *
 * TODO: make these use a generic msg visitor. */


struct typetable_entry *get_or_insert_typeentry(struct upb_strtable *t,
                                                struct upb_msg_field *f)
{
  struct upb_string type_name = upb_issubmsg(f) ? f->ref.msg->fqname :
                                                  upb_type_info[f->type].ctype;
  struct typetable_entry *type_e = upb_strtable_lookup(t, &type_name);
  if(type_e == NULL) {
    struct typetable_entry new_type_e = {
      .e = {.key = type_name}, .field = f, .c_ident = upb_strdup(type_name),
      .values = NULL, .values_size = 0, .values_len = 0,
      .arrays = NULL, .arrays_size = 0, .arrays_len = 0
    };
    to_cident(new_type_e.c_ident);
    upb_strtable_insert(t, &new_type_e.e);
    type_e = upb_strtable_lookup(t, &type_name);
    assert(type_e);
  }
  return type_e;
}

static void add_value(union upb_value value, struct upb_msg_field *f,
                      struct upb_strtable *t)
{
  struct typetable_entry *type_e = get_or_insert_typeentry(t, f);
  if(type_e->values_len == type_e->values_size) {
    type_e->values_size = max(type_e->values_size * 2, 4);
    type_e->values = realloc(type_e->values, sizeof(*type_e->values) * type_e->values_size);
  }
  type_e->values[type_e->values_len++] = value;
}

static void add_submsgs(void *data, struct upb_msg *m, struct upb_strtable *t)
{
  for(uint32_t i = 0; i < m->num_fields; i++) {
    struct upb_msg_field *f = &m->fields[i];
    if(!upb_msg_isset(data, f)) continue;
    union upb_value_ptr p = upb_msg_getptr(data, f);
    if(upb_isarray(f)) {
      if(upb_isstring(f)) continue;  /* Handled by a different code-path. */
      struct upb_array *arr = *p.arr;

      /* Add to our list of arrays for this type. */
      struct typetable_entry *arr_type_e =
          get_or_insert_typeentry(t, f);
      if(arr_type_e->arrays_len == arr_type_e->arrays_size) {
        arr_type_e->arrays_size = max(arr_type_e->arrays_size * 2, 4);
        arr_type_e->arrays = realloc(arr_type_e->arrays,
                                     sizeof(*arr_type_e->arrays)*arr_type_e->arrays_size);
      }
      arr_type_e->arrays[arr_type_e->arrays_len].offset = arr_type_e->values_len;
      arr_type_e->arrays[arr_type_e->arrays_len].len = arr->len;
      arr_type_e->arrays[arr_type_e->arrays_len].ptr = *p.arr;
      arr_type_e->arrays_len++;

      /* Add the individual values in the array. */
      for(uint32_t j = 0; j < arr->len; j++)
        add_value(upb_array_getelement(arr, j, f->type), f, t);

      /* Add submsgs.  We must do this separately so that the msgs in this
       * array are contiguous (and don't have submsgs of the same type
       * interleaved). */
      for(uint32_t j = 0; j < arr->len; j++)
        add_submsgs(*upb_array_getelementptr(arr, j, f->type).msg, f->ref.msg, t);
    } else {
      if(!upb_issubmsg(f)) continue;
      add_value(upb_deref(p, f->type), f, t);
      add_submsgs(*p.msg, f->ref.msg, t);
    }
  }
}

/* write_messages_c emits a .c file that contains the data of a protobuf,
 * serialized as C structures. */
static void write_messages_c(void *data, struct upb_msg *m,
                             char *hfile_name, FILE *stream)
{
  fputs("/* This file was generated by upbc (the upb compiler).  "
        "Do not edit. */\n\n", stream),
  fprintf(stream, "#include \"%s\"\n\n", hfile_name);

  /* Gather all strings into a giant string.  Use a hash to prevent adding the
   * same string more than once. */
  struct upb_strtable strings;
  upb_strtable_init(&strings, 16, sizeof(struct strtable_entry));
  add_strings_from_msg(data, m, &strings);

  int size;
  struct strtable_entry **str_entries = strtable_to_array(&strings, &size);
  /* Sort for nice size and reproduceability. */
  qsort(str_entries, size, sizeof(void*), compare_entries);

  /* Emit strings. */
  fputs("static char strdata[] =\n  \"", stream);
  int col = 2;
  int offset = 0;
  for(int i = 0; i < size; i++) {
    struct upb_string *s = &str_entries[i]->e.key;
    str_entries[i]->offset = offset;
    str_entries[i]->num = i;
    for(uint32_t j = 0; j < s->byte_len; j++) {
      if(++col == 80) {
        fputs("\"\n  \"", stream);
        col = 3;
      }
      fputc(s->ptr[j], stream);
    }
    offset += s->byte_len;
  }
  fputs("\";\n\n", stream);

  fputs("static struct upb_string strings[] = {\n", stream);
  for(int i = 0; i < size; i++) {
    struct strtable_entry *e = str_entries[i];
    fprintf(stream, "  {.ptr = &strdata[%d], .byte_len=%d},\n", e->offset, e->e.key.byte_len);
  }
  fputs("};\n\n", stream);

  /* Gather a list of types for which we are emitting data, and give each msg
   * a unique number within its type. */
  struct upb_strtable types;
  upb_strtable_init(&types, 16, sizeof(struct typetable_entry));
  union upb_value val = {.msg = data};
  /* A fake field to get the recursion going. */
  struct upb_msg_field fake_field = {
      .type = GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_MESSAGE,
      .ref = {.msg = m}
  };
  add_value(val, &fake_field, &types);
  add_submsgs(data, m, &types);

  /* Emit foward declarations for all msgs of all types, and define arrays. */
  fprintf(stream, "/* Forward declarations of messages, and array decls. */\n");
  struct typetable_entry *e = upb_strtable_begin(&types);
  for(; e; e = upb_strtable_next(&types, &e->e)) {
    fprintf(stream, "static " UPB_STRFMT " " UPB_STRFMT "_values[%d];\n\n",
            UPB_STRARG(e->c_ident), UPB_STRARG(e->c_ident), e->values_len);
    if(e->arrays_len > 0) {
      fprintf(stream, "static " UPB_STRFMT " *" UPB_STRFMT "_array_elems[] = {\n",
              UPB_STRARG(e->c_ident), UPB_STRARG(e->c_ident));
      for(int i = 0; i < e->arrays_len; i++) {
        struct array *arr = &e->arrays[i];
        for(int j = 0; j < arr->len; j++)
          fprintf(stream, "    &" UPB_STRFMT "_values[%d],\n", UPB_STRARG(e->c_ident), arr->offset + j);
      }
      fprintf(stream, "};\n");

      int cum_offset = 0;
      fprintf(stream, "static UPB_MSG_ARRAY(" UPB_STRFMT ") " UPB_STRFMT "_arrays[%d] = {\n",
              UPB_STRARG(e->c_ident), UPB_STRARG(e->c_ident), e->arrays_len);
      for(int i = 0; i < e->arrays_len; i++) {
        struct array *arr = &e->arrays[i];
        fprintf(stream, "  {.elements = &" UPB_STRFMT "_array_elems[%d], .len=%d},\n",
                UPB_STRARG(e->c_ident), cum_offset, arr->len);
        cum_offset += arr->len;
      }
      fprintf(stream, "};\n");
    }
  }

  /* Emit definitions. */
  for(e = upb_strtable_begin(&types); e; e = upb_strtable_next(&types, &e->e)) {
    fprintf(stream, "static " UPB_STRFMT " " UPB_STRFMT "_values[%d] = {\n\n",
            UPB_STRARG(e->c_ident), UPB_STRARG(e->c_ident), e->values_len);
    for(int i = 0; i < e->values_len; i++) {
      union upb_value val = e->values[i];
      if(upb_issubmsg(e->field)) {
        struct upb_msg *m = e->field->ref.msg;
        void *msgdata = val.msg;
        /* Print set flags. */
        fprintf(stream, "  {.set_flags = {.bytes = {");
        for(unsigned int j = 0; j < m->set_flags_bytes; j++) {
          fprintf(stream, "0x%02hhx", *(uint8_t*)(val.msg + j));
          if(j < m->set_flags_bytes - 1) fprintf(stream, ", ");
        }
        fprintf(stream, "}},\n");
        /* Print msg data. */
        for(unsigned int j = 0; j < m->num_fields; j++) {
          struct upb_msg_field *f = &m->fields[j];
          google_protobuf_FieldDescriptorProto *fd = m->field_descriptors[j];
          union upb_value val = upb_msg_get(msgdata, f);
          fprintf(stream, "    ." UPB_STRFMT " = ", UPB_STRARG(*fd->name));
          if(!upb_msg_isset(msgdata, f)) {
            fprintf(stream, "0,   /* Not set. */");
          } else if(upb_isstring(f)) {
            if(upb_isarray(f)) {
              fprintf(stderr, "Ack, string arrays are not supported yet!\n");
              exit(1);
            } else {
              struct strtable_entry *str_e = upb_strtable_lookup(&strings, val.str);
              assert(str_e);
              fprintf(stream, "&strings[%d],   /* \"" UPB_STRFMT "\" */",
                      str_e->num, UPB_STRARG(*val.str));
            }
          } else if(upb_isarray(f)) {
            /* Find this submessage in the list of msgs for that type. */
            struct typetable_entry  *type_e = get_or_insert_typeentry(&types, f);
            assert(type_e);
            int arr_num = -1;
            for(int k = 0; k < type_e->arrays_len; k++) {
              if(type_e->arrays[k].ptr == val.arr) {
                arr_num = k;
                break;
              }
            }
            assert(arr_num != -1);
            fprintf(stream, "&" UPB_STRFMT "_arrays[%d],", UPB_STRARG(type_e->c_ident), arr_num);
          } else if(upb_issubmsg(f)) {
            /* Find this submessage in the list of msgs for that type. */
            struct typetable_entry  *type_e = get_or_insert_typeentry(&types, f);
            assert(type_e);
            int msg_num = -1;
            for(int k = 0; k < type_e->values_len; k++) {
              if(type_e->values[k].msg == val.msg) {
                msg_num = k;
                break;
              }
            }
            assert(msg_num != -1);
            fprintf(stream, "&" UPB_STRFMT "_values[%d],", UPB_STRARG(type_e->c_ident), msg_num);
          } else {
            upb_text_printval(f->type, val, stream);
            fprintf(stream, ",");
          }
          fprintf(stream, "\n");
        }
        fprintf(stream, "  },\n");
      } else if(upb_isstring(e->field)) {

      } else {
        /* Non string, non-message data. */
        upb_text_printval(e->field->type, val, stream);
      }
    }
    fprintf(stream, "};\n");
  }
}

const char usage[] =
  "upbc -- upb compiler.\n"
  "upb v0.1  http://blog.reverberate.org/upb/\n"
  "\n"
  "Usage: upbc [options] input-file\n"
  "\n"
  "  -o OUTFILE-BASE    Write to OUTFILE-BASE.h and OUTFILE-BASE.c instead\n"
  "                     of using the input file as a basename.\n"
;

void usage_err(char *err)
{
  fprintf(stderr, "upbc: %s\n\n", err);
  fputs(usage, stderr);
  exit(1);
}

void error(char *err)
{
  fprintf(stderr, "upbc: %s\n\n", err);
  exit(1);
}

int main(int argc, char *argv[])
{
  /* Parse arguments. */
  char *outfile_base = NULL, *input_file = NULL;
  for(int i = 1; i < argc; i++) {
    if(strcmp(argv[i], "-o") == 0) {
      if(++i == argc)
        usage_err("-o must be followed by a FILE-BASE.");
      else if(outfile_base)
        usage_err("-o was specified multiple times.");
      outfile_base = argv[i];
    } else {
      if(input_file)
        usage_err("You can only specify one input file.");
      input_file = argv[i];
    }
  }
  if(!input_file) usage_err("You must specify an input file.");
  if(!outfile_base) outfile_base = input_file;

  /* Read input file. */
  struct upb_string descriptor;
  if(!upb_strreadfile(input_file, &descriptor))
    error("Couldn't read input file.");

  /* Parse input file. */
  struct upb_context c;
  upb_context_init(&c);
  google_protobuf_FileDescriptorSet *fds =
      upb_alloc_and_parse(c.fds_msg, &descriptor, false);
  if(!fds)
    error("Failed to parse input file descriptor.");
  if(!upb_context_addfds(&c, fds))
    error("Failed to resolve symbols in descriptor.\n");

  /* Emit output files. */
  const int maxsize = 256;
  char h_filename[maxsize], c_filename[maxsize];
  if(snprintf(h_filename, maxsize, "%s.h", outfile_base) >= maxsize ||
     snprintf(c_filename, maxsize, "%s.c", outfile_base) >= maxsize)
    error("File base too long.\n");

  FILE *h_file = fopen(h_filename, "w"), *c_file = fopen(c_filename, "w");
  if(!h_file || !c_file)
    error("Failed to open output file(s)");

  int symcount;
  struct upb_symtab_entry **entries = strtable_to_array(&c.symtab, &symcount);
  write_h(entries, symcount, h_filename, h_file);
  write_messages_c(fds, c.fds_msg, h_filename, c_file);
  upb_context_free(&c);
  upb_strfree(descriptor);
  fclose(h_file);
  fclose(c_file);

  return 0;
}
