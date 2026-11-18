/* One-shot ASN.1 DER. Included after the package's shared output helper. */
#include <errno.h>
static SP_TLS sp_x509_out sp_asn1_out;

static ASN1_OBJECT *sp_asn1_object(const char *name, int required) {
  if (!name) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  if (memchr(name, 0, sp_str_byte_len(name)))
    sp_raise_cls("ArgumentError", "string contains null byte");
  ERR_clear_error();
  ASN1_OBJECT *object = OBJ_txt2obj(name, 0);
  if (!object && required) sp_raise_cls("OpenSSL::ASN1::ASN1Error", "invalid OBJECT ID");
  return object;
}

const char *sp_openssl_asn1_name(const char *name, sp_int form) {
  ASN1_OBJECT *object = sp_asn1_object(name, !form);
  int nid = object ? OBJ_obj2nid(object) : NID_undef, length;
  const char *text = form == 1 ? OBJ_nid2sn(nid) : OBJ_nid2ln(nid);
  if (!form) length = OBJ_obj2txt(NULL, 0, object, 1);
  else length = nid == NID_undef || !text ? 0 : (int)strlen(text);
  int ok = length >= 0 && length < INT_MAX && sp_x509_room(&sp_asn1_out, (size_t)length + 1);
  if (ok && !form) OBJ_obj2txt(sp_asn1_out.p, length + 1, object, 1);
  else if (ok && length) memcpy(sp_asn1_out.p, text, (size_t)length);
  ASN1_OBJECT_free(object);
  if (!ok) sp_raise_cls("OpenSSL::ASN1::ASN1Error", "OBJ_obj2txt failed");
  sp_ffi_bin_len = length;
  return sp_asn1_out.p;
}

const char *sp_openssl_asn1_der(sp_int tag, const char *data, sp_int unused) {
  if (!data) sp_raise_cls("TypeError", "no implicit conversion of nil into String");
  size_t size = sp_str_byte_len(data);
  if (size > INT_MAX - 16) sp_raise_cls("RangeError", "ASN.1 value too large");
  ASN1_OBJECT *object = NULL;
  ASN1_INTEGER *integer = NULL;
  ASN1_BIT_STRING *bits = NULL;
  BIGNUM *bn = NULL;
  int length = -1;
  ERR_clear_error();
  if (tag == V_ASN1_OBJECT) {
    object = sp_asn1_object(data, 1);
    length = i2d_ASN1_OBJECT(object, NULL);
  } else if (tag == V_ASN1_INTEGER) {
    if (BN_dec2bn(&bn, data)) integer = BN_to_ASN1_INTEGER(bn, NULL);
    if (integer) length = i2d_ASN1_INTEGER(integer, NULL);
  } else if (tag == V_ASN1_BIT_STRING) {
    if (unused < 0 || unused > 7)
      sp_raise_cls("OpenSSL::ASN1::ASN1Error", "unused_bits for a bitstring value must be in the range 0 to 7");
    bits = ASN1_BIT_STRING_new();
    if (bits && ASN1_BIT_STRING_set(bits, (unsigned char *)data, (int)size)) {
      bits->flags = (bits->flags & ~(ASN1_STRING_FLAG_BITS_LEFT | 7)) | ASN1_STRING_FLAG_BITS_LEFT | unused;
      length = i2d_ASN1_BIT_STRING(bits, NULL);
    }
  } else if (tag == V_ASN1_SEQUENCE) length = ASN1_object_size(1, (int)size, V_ASN1_SEQUENCE);
  int ok = length > 0 && sp_x509_room(&sp_asn1_out, (size_t)length);
  if (ok) {
    unsigned char *out = (unsigned char *)sp_asn1_out.p;
    if (object) ok = i2d_ASN1_OBJECT(object, &out) == length;
    else if (integer) ok = i2d_ASN1_INTEGER(integer, &out) == length;
    else if (bits) ok = i2d_ASN1_BIT_STRING(bits, &out) == length;
    else {
      ASN1_put_object(&out, 1, (int)size, V_ASN1_SEQUENCE, V_ASN1_UNIVERSAL);
      memcpy(out, data, size);
    }
  }
  ASN1_OBJECT_free(object);
  ASN1_INTEGER_free(integer);
  ASN1_BIT_STRING_free(bits);
  BN_free(bn);
  if (!ok) sp_raise_cls("OpenSSL::ASN1::ASN1Error", "ASN.1 encoding failed");
  sp_ffi_bin_len = length;
  return sp_asn1_out.p;
}

/* Keep the numeric value across the FFI boundary, including boxed bigints.
   Runtime scalar formatting bypasses Ruby's overridable Integer#to_s. */
const char *sp_openssl_asn1_integer(sp_RbVal value) {
  SP_GC_ROOT_RBVAL(value);
  const char *decimal = sp_openssl_integer_decimal(value);
  return sp_openssl_asn1_der(V_ASN1_INTEGER, decimal, 0);
}

/* NUM2INT semantics for numeric values; Ruby handles other objects' to_int. */
sp_RbVal sp_openssl_asn1_unused_bits(sp_RbVal value) {
  SP_GC_ROOT_RBVAL(value);
  long number;
  char message[128];
  if (value.tag == SP_TAG_FLT) {
    double f = value.v.f;
    if (!isfinite(f)) sp_raise_cls("RangeError", isnan(f) ? "float NaN out of range of integer" :
      f > 0 ? "float Inf out of range of integer" : "float -Inf out of range of integer");
    if (f < (double)LONG_MIN || f >= -(double)LONG_MIN) {
      snprintf(message, sizeof message, "float %.10g out of range of integer", f);
      sp_raise_cls("RangeError", message);
    }
    number = (long)f;
  } else if (value.tag == SP_TAG_INT) number = (long)value.v.i;
  else if (value.tag == SP_TAG_BIGINT) {
    const char *decimal = sp_openssl_integer_decimal(value);
    errno = 0;
    number = strtol(decimal, NULL, 10);
    if (errno == ERANGE) sp_raise_cls("RangeError", "bignum too big to convert into 'long'");
  } else return sp_box_nil();
  if (number < INT_MIN || number > INT_MAX) {
    snprintf(message, sizeof message, "integer %ld too %s to convert to 'int'", number,
             number < 0 ? "small" : "big");
    sp_raise_cls("RangeError", message);
  }
  return sp_box_int((sp_int)number);
}

static void sp_asn1_decode_error(const char *prefix) {
  const char *reason = ERR_reason_error_string(ERR_peek_last_error());
  char message[256];
  snprintf(message, sizeof message, "%s%s%s", prefix, *prefix ? ": " : "",
           reason ? reason : "ASN.1 decoding failed");
  ERR_clear_error();
  sp_raise_cls("OpenSSL::ASN1::ASN1Error", message);
}

static void sp_asn1_decode_unavailable(const char *form, int tag) {
  const char *type = tag == V_ASN1_INTEGER ? "Integer" : tag == V_ASN1_BIT_STRING ? "BitString" :
    tag == V_ASN1_OBJECT ? "ObjectId" : tag == V_ASN1_SEQUENCE ? "Sequence" :
    tag == V_ASN1_OCTET_STRING ? "OctetString" : tag == V_ASN1_NULL ? "Null" :
    tag == V_ASN1_SET ? "Set" : tag == V_ASN1_BOOLEAN ? "Boolean" : "tag";
  char message[160];
  snprintf(message, sizeof message, "ASN.1 %s%s%s (tag %d) is unavailable",
           form, *form ? " " : "", type, tag);
  sp_raise_cls("NotImplementedError", message);
}

/* Every span is bounded by its parent, not just by the input String. */
sp_int sp_openssl_asn1_decode_info(const char *der, sp_int offset, sp_int limit, sp_int part) {
  size_t size = der ? sp_str_byte_len(der) : 0;
  if (!der || offset < 0 || limit < offset || (size_t)limit > size)
    sp_raise_cls("OpenSSL::ASN1::ASN1Error", "invalid ASN.1 bounds");
  const unsigned char *start = (const unsigned char *)der + offset, *p = start;
  long length = 0;
  int tag, klass;
  ERR_clear_error();
  int flags = ASN1_get_object(&p, &length, &tag, &klass, (long)(limit - offset));
  if (flags & 0x80) sp_asn1_decode_error("");
  if (flags & 1) sp_asn1_decode_unavailable("indefinite-length", tag);
  if (length < 0 || length > limit - offset - (p - start))
    sp_raise_cls("OpenSSL::ASN1::ASN1Error", "too long");
  if ((*start & 31) == 31) sp_asn1_decode_unavailable("high-tag-number", tag);
  if (klass != V_ASN1_UNIVERSAL)
    sp_asn1_decode_unavailable(klass == V_ASN1_CONTEXT_SPECIFIC ? "context-specific" :
                             klass == V_ASN1_APPLICATION ? "application" : "private", tag);
  if (tag == V_ASN1_SEQUENCE && !(flags & V_ASN1_CONSTRUCTED))
    sp_asn1_decode_unavailable("primitive", tag);
  if ((flags & V_ASN1_CONSTRUCTED) && tag != V_ASN1_SEQUENCE)
    sp_asn1_decode_unavailable("constructed", tag);
  if (tag != V_ASN1_SEQUENCE && tag != V_ASN1_INTEGER && tag != V_ASN1_OBJECT && tag != V_ASN1_BIT_STRING)
    sp_asn1_decode_unavailable("", tag);
  sp_int end = offset + (p - start) + length;
  if (part == 3 && end != limit) {
    char message[160];
    snprintf(message, sizeof message,
             "Type mismatch. Total bytes read: %ld Bytes available: %ld Offset: %ld",
             (long)end, (long)limit, (long)end);
    sp_raise_cls("OpenSSL::ASN1::ASN1Error", message);
  }
  return part == 0 ? tag : part == 1 ? offset + (p - start) : end;
}

sp_RbVal sp_openssl_asn1_decode_integer(const char *der) {SP_GC_ROOT_STR(der);
  const unsigned char *p = (const unsigned char *)der;
  ERR_clear_error();
  ASN1_INTEGER *integer = d2i_ASN1_INTEGER(NULL, &p, (long)sp_str_byte_len(der));
  if (!integer) sp_asn1_decode_error("");
  BIGNUM *bn = ASN1_INTEGER_to_BN(integer, NULL);
  ASN1_INTEGER_free(integer);
  if (!bn) sp_asn1_decode_error("");
  return sp_openssl_bn_answer(bn);
}

/* A byte prefix carries OID name presence or the bit string's unused bits. */
const char *sp_openssl_asn1_decode_value(const char *der, sp_int tag) {
  const unsigned char *p = (const unsigned char *)der;
  long size = (long)sp_str_byte_len(der);
  int length = -1, ok = 0;
  ERR_clear_error();
  if (tag == V_ASN1_OBJECT) {
    ASN1_OBJECT *object = d2i_ASN1_OBJECT(NULL, &p, size);
    if (!object) sp_asn1_decode_error("d2i_ASN1_OBJECT");
    int nid = OBJ_obj2nid(object);
    const char *name = nid == NID_undef ? NULL : OBJ_nid2sn(nid);
    length = name ? (int)strlen(name) : OBJ_obj2txt(NULL, 0, object, 1);
    ok = length >= 0 && length < INT_MAX - 1 && sp_x509_room(&sp_asn1_out, (size_t)length + 2);
    if (ok) {
      sp_asn1_out.p[0] = name != NULL;
      if (name) memcpy(sp_asn1_out.p + 1, name, (size_t)length);
      else OBJ_obj2txt(sp_asn1_out.p + 1, length + 1, object, 1);
      length++;
    }
    ASN1_OBJECT_free(object);
  } else {
    ASN1_BIT_STRING *bits = d2i_ASN1_BIT_STRING(NULL, &p, size);
    if (!bits) sp_asn1_decode_error("d2i_ASN1_BIT_STRING");
    length = ASN1_STRING_length(bits);
    int unused = bits->flags & 7;
    const unsigned char *bytes = ASN1_STRING_get0_data(bits);
    ok = length >= 0 && length < INT_MAX &&
         sp_x509_room(&sp_asn1_out, (size_t)length + 1);
    if (ok) {
      sp_asn1_out.p[0] = (char)unused;
      if (length) memcpy(sp_asn1_out.p + 1, bytes, (size_t)length);
      length++;
    }
    ASN1_BIT_STRING_free(bits);
  }
  if (!ok) sp_raise_cls("OpenSSL::ASN1::ASN1Error", "ASN.1 decoding failed");
  sp_ffi_bin_len = length;
  return sp_asn1_out.p;
}
