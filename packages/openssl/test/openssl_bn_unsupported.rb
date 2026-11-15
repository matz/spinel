# spinel: not-cruby -- The documented BN subset refuses unsupported numeric operations.
# CRuby provides these methods; native rejection is intentional, including Object fallbacks.
require "openssl"
number = OpenSSL::BN.new("7")
begin
  number.__send__(:%)
  puts "accepted %"
rescue NoMethodError
  puts "%: NoMethodError"
end
begin
  number.__send__(:*)
  puts "accepted *"
rescue NoMethodError
  puts "*: NoMethodError"
end
begin
  number.__send__(:**)
  puts "accepted **"
rescue NoMethodError
  puts "**: NoMethodError"
end
begin
  number.__send__(:+@)
  puts "accepted +@"
rescue NoMethodError
  puts "+@: NoMethodError"
end
begin
  number.__send__(:-@)
  puts "accepted -@"
rescue NoMethodError
  puts "-@: NoMethodError"
end
begin
  number.__send__(:/)
  puts "accepted /"
rescue NoMethodError
  puts "/: NoMethodError"
end
begin
  number.__send__(:<<)
  puts "accepted <<"
rescue NoMethodError
  puts "<<: NoMethodError"
end
begin
  number.__send__(:>>)
  puts "accepted >>"
rescue NoMethodError
  puts ">>: NoMethodError"
end
begin
  number.__send__(:abs)
  puts "accepted abs"
rescue NoMethodError
  puts "abs: NoMethodError"
end
begin
  number.__send__(:bit_set?)
  puts "accepted bit_set?"
rescue NoMethodError
  puts "bit_set?: NoMethodError"
end
begin
  number.__send__(:clear_bit!)
  puts "accepted clear_bit!"
rescue NoMethodError
  puts "clear_bit!: NoMethodError"
end
begin
  number.__send__(:cmp)
  puts "accepted cmp"
rescue NoMethodError
  puts "cmp: NoMethodError"
end
begin
  number.__send__(:copy)
  puts "accepted copy"
rescue NoMethodError
  puts "copy: NoMethodError"
end
begin
  number.__send__(:eql?, number)
  puts "accepted eql?"
rescue NoMethodError
  puts "eql?: NoMethodError"
end
begin
  number.__send__(:gcd)
  puts "accepted gcd"
rescue NoMethodError
  puts "gcd: NoMethodError"
end
begin
  number.__send__(:get_flags)
  puts "accepted get_flags"
rescue NoMethodError
  puts "get_flags: NoMethodError"
end
begin
  number.__send__(:hash)
  puts "accepted hash"
rescue NoMethodError
  puts "hash: NoMethodError"
end
begin
  number.__send__(:lshift!)
  puts "accepted lshift!"
rescue NoMethodError
  puts "lshift!: NoMethodError"
end
begin
  number.__send__(:mask_bits!)
  puts "accepted mask_bits!"
rescue NoMethodError
  puts "mask_bits!: NoMethodError"
end
begin
  number.__send__(:mod_add)
  puts "accepted mod_add"
rescue NoMethodError
  puts "mod_add: NoMethodError"
end
begin
  number.__send__(:mod_exp)
  puts "accepted mod_exp"
rescue NoMethodError
  puts "mod_exp: NoMethodError"
end
begin
  number.__send__(:mod_inverse)
  puts "accepted mod_inverse"
rescue NoMethodError
  puts "mod_inverse: NoMethodError"
end
begin
  number.__send__(:mod_mul)
  puts "accepted mod_mul"
rescue NoMethodError
  puts "mod_mul: NoMethodError"
end
begin
  number.__send__(:mod_sqr)
  puts "accepted mod_sqr"
rescue NoMethodError
  puts "mod_sqr: NoMethodError"
end
begin
  number.__send__(:mod_sqrt)
  puts "accepted mod_sqrt"
rescue NoMethodError
  puts "mod_sqrt: NoMethodError"
end
begin
  number.__send__(:mod_sub)
  puts "accepted mod_sub"
rescue NoMethodError
  puts "mod_sub: NoMethodError"
end
begin
  number.__send__(:negative?)
  puts "accepted negative?"
rescue NoMethodError
  puts "negative?: NoMethodError"
end
begin
  number.__send__(:num_bits)
  puts "accepted num_bits"
rescue NoMethodError
  puts "num_bits: NoMethodError"
end
begin
  number.__send__(:num_bytes)
  puts "accepted num_bytes"
rescue NoMethodError
  puts "num_bytes: NoMethodError"
end
begin
  number.__send__(:odd?)
  puts "accepted odd?"
rescue NoMethodError
  puts "odd?: NoMethodError"
end
begin
  number.__send__(:one?)
  puts "accepted one?"
rescue NoMethodError
  puts "one?: NoMethodError"
end
begin
  number.__send__(:pretty_print)
  puts "accepted pretty_print"
rescue NoMethodError
  puts "pretty_print: NoMethodError"
end
begin
  number.__send__(:prime?)
  puts "accepted prime?"
rescue NoMethodError
  puts "prime?: NoMethodError"
end
begin
  number.__send__(:prime_fasttest?)
  puts "accepted prime_fasttest?"
rescue NoMethodError
  puts "prime_fasttest?: NoMethodError"
end
begin
  number.__send__(:rshift!)
  puts "accepted rshift!"
rescue NoMethodError
  puts "rshift!: NoMethodError"
end
begin
  number.__send__(:set_bit!)
  puts "accepted set_bit!"
rescue NoMethodError
  puts "set_bit!: NoMethodError"
end
begin
  number.__send__(:set_flags)
  puts "accepted set_flags"
rescue NoMethodError
  puts "set_flags: NoMethodError"
end
begin
  number.__send__(:sqr)
  puts "accepted sqr"
rescue NoMethodError
  puts "sqr: NoMethodError"
end
begin
  number.__send__(:to_bn)
  puts "accepted to_bn"
rescue NoMethodError
  puts "to_bn: NoMethodError"
end
begin
  number.__send__(:to_int)
  puts "accepted to_int"
rescue NoMethodError
  puts "to_int: NoMethodError"
end
begin
  number.__send__(:ucmp)
  puts "accepted ucmp"
rescue NoMethodError
  puts "ucmp: NoMethodError"
end
begin
  number.__send__(:zero?)
  puts "accepted zero?"
rescue NoMethodError
  puts "zero?: NoMethodError"
end
begin
  { number => 1 }
  puts "accepted hash key"
rescue NoMethodError
  puts "hash-key: NoMethodError"
end
begin
  number.<(3)
  puts "accepted < form 0"
rescue NoMethodError
  puts "< form 0: NoMethodError"
end
begin
  number.send(:<, 3)
  puts "accepted < form 1"
rescue NoMethodError
  puts "< form 1: NoMethodError"
end
begin
  number.__send__(:<, 3)
  puts "accepted < form 2"
rescue NoMethodError
  puts "< form 2: NoMethodError"
end
begin
  number.<=(3)
  puts "accepted <= form 0"
rescue NoMethodError
  puts "<= form 0: NoMethodError"
end
begin
  number.send(:<=, 3)
  puts "accepted <= form 1"
rescue NoMethodError
  puts "<= form 1: NoMethodError"
end
begin
  number.__send__(:<=, 3)
  puts "accepted <= form 2"
rescue NoMethodError
  puts "<= form 2: NoMethodError"
end
begin
  number.>(3)
  puts "accepted > form 0"
rescue NoMethodError
  puts "> form 0: NoMethodError"
end
begin
  number.send(:>, 3)
  puts "accepted > form 1"
rescue NoMethodError
  puts "> form 1: NoMethodError"
end
begin
  number.__send__(:>, 3)
  puts "accepted > form 2"
rescue NoMethodError
  puts "> form 2: NoMethodError"
end
begin
  number.>=(3)
  puts "accepted >= form 0"
rescue NoMethodError
  puts ">= form 0: NoMethodError"
end
begin
  number.send(:>=, 3)
  puts "accepted >= form 1"
rescue NoMethodError
  puts ">= form 1: NoMethodError"
end
begin
  number.__send__(:>=, 3)
  puts "accepted >= form 2"
rescue NoMethodError
  puts ">= form 2: NoMethodError"
end
begin
  number.clamp(3, 11)
  puts "accepted clamp form 0"
rescue NoMethodError
  puts "clamp form 0: NoMethodError"
end
begin
  number.send(:clamp, 3, 11)
  puts "accepted clamp form 1"
rescue NoMethodError
  puts "clamp form 1: NoMethodError"
end
begin
  number.__send__(:clamp, 3, 11)
  puts "accepted clamp form 2"
rescue NoMethodError
  puts "clamp form 2: NoMethodError"
end
begin
  number.between?(3, 11)
  puts "accepted between? form 0"
rescue NoMethodError
  puts "between? form 0: NoMethodError"
end
begin
  number.send(:between?, 3, 11)
  puts "accepted between? form 1"
rescue NoMethodError
  puts "between? form 1: NoMethodError"
end
begin
  number.__send__(:between?, 3, 11)
  puts "accepted between? form 2"
rescue NoMethodError
  puts "between? form 2: NoMethodError"
end
begin
  number.coerce(3)
  puts "accepted coerce form 0"
rescue NoMethodError
  puts "coerce form 0: NoMethodError"
end
begin
  number.send(:coerce, 3)
  puts "accepted coerce form 1"
rescue NoMethodError
  puts "coerce form 1: NoMethodError"
end
begin
  number.__send__(:coerce, 3)
  puts "accepted coerce form 2"
rescue NoMethodError
  puts "coerce form 2: NoMethodError"
end
begin
  number.+(3)
  puts "accepted + form 0"
rescue NoMethodError
  puts "+ form 0: NoMethodError"
end
begin
  number.send(:+, 3)
  puts "accepted + form 1"
rescue NoMethodError
  puts "+ form 1: NoMethodError"
end
begin
  number.__send__(:+, 3)
  puts "accepted + form 2"
rescue NoMethodError
  puts "+ form 2: NoMethodError"
end
begin
  number.-(3)
  puts "accepted - form 0"
rescue NoMethodError
  puts "- form 0: NoMethodError"
end
begin
  number.send(:-, 3)
  puts "accepted - form 1"
rescue NoMethodError
  puts "- form 1: NoMethodError"
end
begin
  number.__send__(:-, 3)
  puts "accepted - form 2"
rescue NoMethodError
  puts "- form 2: NoMethodError"
end
begin
  3 + number
  puts "accepted Integer arithmetic 0"
rescue NoMethodError
  puts "Integer arithmetic 0: NoMethodError"
end
begin
  3 - number
  puts "accepted Integer arithmetic 1"
rescue NoMethodError
  puts "Integer arithmetic 1: NoMethodError"
end
