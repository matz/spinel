# A top-level respond_to_missing? under a condition that did not run: main
# answers Kernel's false, as it does without the def.
if ARGV.size > 0
  def respond_to_missing?(n, all = false) = n == :a
end
p respond_to?(:a)
p Object.new.respond_to?(:a)
