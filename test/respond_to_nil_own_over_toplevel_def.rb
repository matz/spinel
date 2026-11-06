# A top-level def is a private method of Object, but NilClass's own public
# method of the name sits below it: nil still answers respond_to? true.
def rationalize(*) = 5
def to_a = []
private def helper = 1
x = [nil, 1].first
p x.respond_to?(:rationalize), x.respond_to?(:to_a), x.respond_to?(:helper), x.respond_to?(:helper, true)
p [nil, 1].map { |v| v.respond_to?(:to_a) }
def mk(f) = f ? 7 : nil
p mk(false).respond_to?(:to_a), mk(false).respond_to?(:helper), mk(false).respond_to?(:helper, true)
def ms(f) = f ? "s" : nil
p ms(false).respond_to?(:to_a), ms(false).respond_to?(:rationalize)
# an include_all only the run time knows: a private top-level def answers by it
def ia(f) = [f, !f].first
p x.respond_to?(:helper, ia(true)), x.respond_to?(:helper, ia(false))
