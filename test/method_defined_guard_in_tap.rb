# A method_defined? guard in a block that runs once (tap) answers absent before a
# def made after it, as CRuby does.
class C; end
5.tap do
  if C.method_defined?(:m) then puts "defined" else puts "absent" end
end
class C; def m = 1; end
puts(C.method_defined?(:m) ? "defined" : "absent")
