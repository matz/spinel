# As extend_copy_call_meets_superclass_redef, with the reading module
# defined before the class that extends the other one.
module V; def v = 1; end
module M; def f = v; end
class C; extend V; end
class D < C; extend M; end
puts D.f
class C; def self.v = 2; end
puts D.f
