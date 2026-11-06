# nil answers respond_to?(:rationalize) and (:=~) where it comes through a slot
# that may hold nil: a boxed value, a nil-able Integer, Float or String.
x = [nil, 1].first
p x.respond_to?(:rationalize)
p x.respond_to?(:=~)
def mk(f) = f ? 7 : nil
p mk(false).respond_to?(:rationalize)
p mk(true).respond_to?(:rationalize)
p mk(false).respond_to?(:=~)
def ms(f) = f ? "a" : nil
p ms(false).respond_to?(:rationalize)
p ms(true).respond_to?(:rationalize)
p ms(false).respond_to?(:=~)
def mf(f) = f ? 1.5 : nil
p mf(false).respond_to?(:rationalize)
p mf(false).respond_to?(:to_s)
y = [nil, "s", 2, :sym].first
p y.respond_to?(:rationalize)
p [nil, 1].map { |v| v.respond_to?(:rationalize) }
p [nil, :a].map { |v| v.respond_to?(:rationalize) }
p [nil, "s"].map { |v| v.respond_to?(:=~) }
p nil.rationalize, nil.rationalize(0.1)
