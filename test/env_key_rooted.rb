# spinel: gc-stress
# An ENV name the call builds (an interpolation, a method's answer) is a new
# String nothing else holds. ENV[k] = v, ENV.fetch(k, default) and
# ENV.delete(k) held it in a C temp while the value, the default or the
# removed value was allocated, so under SPINEL_GC_STRESS=2 the collection
# freed the name: the variable was set, looked up or unset under freed bytes.
def key(n) = "SPINEL_EK_#{n}"
def reset
  ENV["SPINEL_EK_1"] = "o1"
  ENV["SPINEL_EK_2"] = "o2"
end
4.times { |i| ENV["SPINEL_EK_#{i}"] = "o#{i}" }
p ENV["SPINEL_EK_0"], ENV["SPINEL_EK_3"]
reset
p ENV.fetch(key(1), "d#{1}"), ENV.fetch(key(9), "d#{9}")
reset
p ENV.delete(key(1)), ENV["SPINEL_EK_1"], ENV.delete(key(9))
reset
p ENV.delete(key(2)) { |k| "none #{k}" }, ENV["SPINEL_EK_2"], ENV.delete(key(8)) { |k| "none #{k}" }
%w[SPINEL_EK_0 SPINEL_EK_1 SPINEL_EK_2 SPINEL_EK_3].each { |k| ENV.delete(k) }
p (0...4).map { |i| ENV.key?("SPINEL_EK_#{i}") }
