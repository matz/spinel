# A builtin receiver's Method made from a run-time name: its wrapper dispatches
# every name the program spells, compare_by_identity among them, and that arm
# raises only when it runs, as a probed arm would.

require "set"
Set.new.compare_by_identity
def keywords?(record, subject)
  record.method(subject).parameters
end
p keywords?({ 1 => 2 }, :size)
p keywords?("x", "size")
