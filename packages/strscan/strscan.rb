# Spinel bundled `strscan` -- a carried-C spin package (Path B typed object).
#
# StringScanner is a native-bound class: the struct and every method live in
# this package's C (sp_strscan.c, linked only when `require "strscan"`
# appears), and the declarations below are the compiler's entire knowledge of
# it. `:regexp` args accept a regex literal at the call site (compiled to the
# generated program's sp_re_pat_<n> pattern); `:self` returns the receiver.
# Each pattern method is bound twice: a String pattern (the `[:string]`
# binding) matches its text literally.
# `:string?` returns are nullable (NULL stays a nil-able string, matching the
# scanner's no-match results).
module StringScannerPackage
  native_lib "strscan"
  native_obj "packages/strscan/sp_strscan.o"

  native_struct "StringScanner", "sp_StringScanner"
  native_new [:string], "sp_StringScanner_new"

  native_method :scan,       [:regexp], :nstring, "sp_StringScanner_scan"
  native_method :scan,       [:string], :nstring, "sp_StringScanner_scan_str"
  native_method :check,      [:regexp], :nstring, "sp_StringScanner_check"
  native_method :check,      [:string], :nstring, "sp_StringScanner_check_str"
  native_method :scan_until, [:regexp], :nstring, "sp_StringScanner_scan_until"
  native_method :scan_until, [:string], :nstring, "sp_StringScanner_scan_until_str"
  native_method :check_until, [:regexp], :nstring, "sp_StringScanner_check_until"
  native_method :check_until, [:string], :nstring, "sp_StringScanner_check_until_str"
  native_method :match?,     [:regexp], :any,     "sp_StringScanner_match_q"
  native_method :match?,     [:string], :any,     "sp_StringScanner_match_q_str"
  native_method :skip,       [:regexp], :any,     "sp_StringScanner_skip"
  native_method :skip,       [:string], :any,     "sp_StringScanner_skip_str"
  native_method :exist?,     [:regexp], :any,     "sp_StringScanner_exist_q"
  native_method :exist?,     [:string], :any,     "sp_StringScanner_exist_q_str"
  native_method :skip_until, [:regexp], :any,     "sp_StringScanner_skip_until"
  native_method :skip_until, [:string], :any,     "sp_StringScanner_skip_until_str"
  native_method :dup,        [], :self,           "sp_StringScanner_dup"
  native_method :clone,      [], :self,           "sp_StringScanner_dup"
  native_method :[],         [:int], :nstring,    "sp_StringScanner_aref"
  native_method :[],         [:string], :nstring, "sp_StringScanner_aref_name"
  native_method :[],         [:any], :nstring,    "sp_StringScanner_aref_any"
  native_method :matched,    [], :nstring,        "sp_StringScanner_matched"
  native_method :matched?,   [], :bool,           "sp_StringScanner_matched_p"
  native_method :matched_size, [], :any,          "sp_StringScanner_matched_size"
  native_method :captures,   [], :any,            "sp_StringScanner_captures"
  native_method :named_captures, [], :any,        "sp_StringScanner_named_captures"
  native_method :values_at,  [:rest], :any,       "sp_StringScanner_values_at"
  native_method :pos,        [], :int,            "sp_StringScanner_pos"
  native_method :charpos,    [], :int,            "sp_StringScanner_charpos"
  native_method :pos=,       [:int], :int,        "sp_StringScanner_pos_set"
  native_method :eos?,       [], :bool,           "sp_StringScanner_eos_p"
  native_method :getch,      [], :nstring,        "sp_StringScanner_getch"
  native_method :peek,       [:int], :string,     "sp_StringScanner_peek"
  native_method :get_byte,   [], :nstring,        "sp_StringScanner_get_byte"
  native_method :scan_byte,  [], :any,            "sp_StringScanner_scan_byte"
  native_method :peek_byte,  [], :any,            "sp_StringScanner_peek_byte"
  native_method :unscan,     [], :self,           "sp_StringScanner_unscan"
  native_method :rest,       [], :string,         "sp_StringScanner_rest"
  native_method :rest_size,  [], :int,            "sp_StringScanner_rest_size"
  native_method :rest?,      [], :bool,           "sp_StringScanner_rest_p"
  native_method :terminate,  [], :self,           "sp_StringScanner_terminate"
  native_method :string,     [], :string,         "sp_StringScanner_string"
  native_method :string=,    [:string], :string,  "sp_StringScanner_string_set"
  native_method :pre_match,  [], :nstring,        "sp_StringScanner_pre_match"
  native_method :post_match, [], :nstring,        "sp_StringScanner_post_match"
  native_method :reset,      [], :self,           "sp_StringScanner_reset"
end
