# spinel: not-cruby -- ffi_source / ffi_func are spinel's; the .expected is authored (strtod's answer in every mode)
# JSON.parse reads a short Float with one division or product, and a caller
# can change the rounding mode through FFI: under each of the four modes the
# parsed value is the one libc's strtod gives, negative values included (the
# sign goes on before the scaling, so the signed value is what is rounded).
require "json"
module Fe
  ffi_source "#include <fenv.h>\n#include <stdlib.h>\nint fe_mode(int i){int m[4]={FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};return fesetround(m[i]);}\ndouble fe_strtod(const char*s){return strtod(s,0);}\n"
  ffi_func :fe_mode, [:int], :int
  ffi_func :fe_strtod, [:str], :double
end
strs = %w[-0.1 0.1 -0.3 0.3 -2.675 -1.15 -123456789.012345 -1e-22 -7e21 -0.000123456789 4.35 -8.41 -0.7 0.7]
seed = 5
600.times do |i|
  seed = (seed * 75 + 74) % 65537
  a = seed
  seed = (seed * 75 + 74) % 65537
  strs << "#{i.odd? ? "-" : ""}#{a}.#{(seed % 1000000).to_s.rjust(6, "0")}e#{i % 9 - 4}"
end
bad = 0
4.times do |mode|
  Fe.fe_mode(mode)
  strs.each do |s|
    a = JSON.parse("[#{s}]")[0]
    b = Fe.fe_strtod(s)
    bad += 1 if [a].pack("G") != [b].pack("G")
  end
  Fe.fe_mode(0)
  puts "mode #{mode}: #{bad} differ from strtod"
  bad = 0
end
