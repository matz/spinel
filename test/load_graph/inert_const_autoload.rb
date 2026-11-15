# A `when` value of another family than the subject was dropped unevaluated
# when it was a constant read, whatever the read ran: an autoloaded constant
# (or `Mod::LAZY`) loaded its file only at the next use, and a constant that
# may be undefined never raised its NameError. A defined constant stays inert.
autoload :LAZY_A, File.join(__dir__, "inert_const_lazy_a_lib")
autoload :LAZY_B, File.join(__dir__, "inert_const_lazy_b_lib")
module Mod; autoload :LAZY, File.join(__dir__, "inert_const_lazy_m_lib"); end
PLAIN = 3

case "x"
when LAZY_A then p :int
else p :none
end
p LAZY_A

case "x"
when Mod::LAZY then p :int
else p :none
end
p Mod::LAZY

case "x"
when PLAIN then p :int
else p :none
end

case 5
when Integer then p :first
when LAZY_B then p :never
end
puts "end"

case "x"
when "y", LAZY_B then p :int
else p :none
end
p LAZY_B

begin
  case "x"
  when MAYBE_UNDEF then p :int
  else p :none
  end
rescue NameError => e
  p e.class
end
MAYBE_UNDEF = 1 if false
