class C; end
pr = proc do
  if C.method_defined?(:m)
    puts "defined"
  else
    puts "absent"
  end
end
class C; def m = 1; end
pr.call
