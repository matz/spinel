# A nil narrowing that type inference itself reads: the guarded `x` is the
# argument the other lambda's parameter is typed from, inside the fixpoint.
inner = ->(v) { puts v + 1 }
outer = ->(x) { inner.call(x) if x }
outer.call(nil)
outer.call(2)
