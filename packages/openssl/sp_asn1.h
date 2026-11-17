/* One-shot ASN.1 encoding. Included after the package's shared output helper. */
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
