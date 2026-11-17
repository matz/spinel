# A method_defined? guard after a loop that has ended answers absent before a def
# made after it, then present.
class A; end
i = 0
while i < 2
  i += 1
end
if A.method_defined?(:m) then puts "defined" else puts "absent" end
class A; def m = 1; end
if A.method_defined?(:m) then puts "defined" else puts "absent" end
