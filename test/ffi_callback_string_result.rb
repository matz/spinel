# spinel: not-cruby -- ffi_callback and ffi_func are Spinel's own
module Reader
  ffi_source <<~'C'
    typedef const char *(*read_cb)(long);
    int callback_bytes(read_cb fn, long flag) {
      const char *bytes = fn(flag);
      if (!flag) return !bytes && sp_ffi_bin_len == 0 ? 0 : -2;
      if (!bytes || sp_ffi_bin_len != 3 || bytes[0] != 'a' || bytes[1] != 0) return -2;
      return sp_ffi_bin_len * 1000 + (unsigned char)bytes[2];
    }
    int callback_text(read_cb fn, long flag) {
      const char *text = fn(flag);
      return text ? (unsigned char)text[0] : -1;
    }
  C
  ffi_callback :read_cb, [:long], :str
  ffi_callback :binary_cb, [:long], :binstr
  ffi_func :callback_bytes, [:binary_cb, :long], :int
  ffi_func :callback_text, [:read_cb, :long], :int
end

def answer(flag)
  return nil if flag == 0
  "é"
end

p Reader.callback_text(method(:answer), 1)
p Reader.callback_text(method(:answer), 0)

def binary_answer(flag)
  return nil if flag == 0
  "a\0\xff".b
end

p Reader.callback_bytes(method(:binary_answer), 1)
p Reader.callback_bytes(method(:binary_answer), 0)
