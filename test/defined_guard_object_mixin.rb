# A top-level `defined?(X)` guard sees a constant of a module mixed into
# Object from a `class Object` body, and a writer in the other arm of an if
# that a rescue's `retry` runs again.
module GuardMixin
  MixedConst = :mixed
end
class Object
  include GuardMixin
end
puts "missing mixed" unless defined?(MixedConst)
p MixedConst

tries = 0
begin
  tries += 1
  if tries == 1
    RetriedConst = :first
    raise "again"
  else
    puts "missing retried" unless defined?(RetriedConst)
  end
rescue RuntimeError
  retry
end
p [tries, RetriedConst]
