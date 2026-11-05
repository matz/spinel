# respond_to? on an Integer, Float, String, Symbol, nil or true answers false
# for a name CRuby's class lacks (to_a, to_h, length, to_str), even where
# the compiler has a conversion of that name for other values.
p 7.respond_to?(:to_a), 7.respond_to?(:to_h), 7.respond_to?(:length), 7.respond_to?(:to_str)
p 7.5.respond_to?(:to_a), 7.5.respond_to?(:size), 7.5.respond_to?(:ord)
p "s".respond_to?(:to_int), "s".respond_to?(:abs), "s".respond_to?(:to_s)
p :sy.respond_to?(:to_str), :sy.respond_to?(:gsub), :sy.respond_to?(:to_proc)
p true.respond_to?(:to_i), true.respond_to?(:size), nil.respond_to?(:to_a)
def mk(f) = f ? 7 : nil
p mk(true).respond_to?(:to_a), mk(false).respond_to?(:to_a)
p [1].respond_to?(:each), (1..2).respond_to?(:each_slice)
