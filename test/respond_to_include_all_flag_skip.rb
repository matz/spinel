# A call of respond_to? with an include_all that is no literal stays a call
# where it must: under `&.` a nil receiver runs neither the name nor the
# flag, and under defined? nothing runs, so it answers "method".
def f = 1
no = !ARGV.empty?
x = ARGV.empty? ? nil : 3
$l = []
p x&.respond_to?(:to_s, ($l << :flag; true)), $l
p defined?(3.respond_to?(:to_s, no)), defined?(respond_to?(:to_s, no)), defined?((3.respond_to?(:f, no)))
p 3.respond_to?(:f, ($l << :flag; true)), $l
