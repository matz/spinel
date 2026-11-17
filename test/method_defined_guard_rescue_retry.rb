# A guard in a rescue clause runs again after `retry`: a def later in the clause has run by the second pass.
class C; end
tries = 0
begin
  raise "again" if tries < 2
  puts "done"
rescue
  tries += 1
  if C.method_defined?(:m)
    seen = "defined"
  else
    seen = "absent"
  end
  puts seen if tries == 2
  class C; def m = 1; end
  retry
end
