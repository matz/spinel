# Built split (--jobs) by infer-test. The macOS 27 SDK headers hold two
# declarations that the split must read: an empty declaration (a `;` that a
# macro leaves alone at file scope, sys/socket.h), and a struct whose `;`
# comes after #pragma lines (SLIST_HEAD in sys/event.h). The lines between
# the `}` and the `;` are `#if 1` / `#endif` here, which the preprocessor
# alone sees: gcc parses a `#pragma GCC diagnostic` as a token of the
# declaration and refuses it there, where Apple clang accepts it, and the
# split's lexer skips any `#` line the same way.
module SdkDecls
  ffi_source <<~C
    #include <stdint.h>
    struct sdk_decls_addr { int family; };
    ;
    struct sdk_decls_list { struct sdk_decls_addr *first; }
    #if 1
    #endif
    ;
    intptr_t sdk_decls_sum(intptr_t value) {
      struct sdk_decls_addr addr = { 1 };
      struct sdk_decls_list list = { &addr };
      return value + list.first->family;
    }
  C
  ffi_func :sdk_decls_sum, [:long], :long
end

puts SdkDecls.sdk_decls_sum(41)
