# StringScanner's share declarations, which the parser reads after
# strscan.rb only under --share-strings (see native_share in the native
# binding DSL, and stringio.share.rb for the kinds).
module StringScannerPackage
  native_struct "StringScanner"   # the class the declarations below are for
  # Each reader answers a new String (sc_substr, a copy out of the source)
  # or nil: one no other name holds, as CRuby's do. The scanner keeps its
  # last match as an offset and a length, not as the String a scan
  # answered, and an empty answer is a new empty String. #string answers
  # the source itself and is left undeclared.
  native_share "[]", "fresh"
  native_share "scan", "fresh"
  native_share "check", "fresh"
  native_share "scan_until", "fresh"
  native_share "check_until", "fresh"
  native_share "matched", "fresh"
  native_share "getch", "fresh"
  native_share "get_byte", "fresh"
  native_share "peek", "fresh"
  native_share "rest", "fresh"
  native_share "pre_match", "fresh"
  native_share "post_match", "fresh"
end
