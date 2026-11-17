# A program class below a builtin exception that declares the builtin's gated
# accessor private or protected (`private :key`) answers respond_to? by that
# visibility: false, and true only when private methods are asked for. The
# inherited-accessor answer was public whatever the class declared.
class PKE < KeyError; private :key; end
class QKE < KeyError; protected :key; end
class OKE < KeyError; end
class UKE < KeyError; public :key; end
e = PKE.new("m")
p e.respond_to?(:key), e.respond_to?(:key, true)
q = QKE.new("m")
p q.respond_to?(:key), q.respond_to?(:key, true)
o = OKE.new("m")
p o.respond_to?(:key), o.respond_to?(:key, true)
u = UKE.new("m")
p u.respond_to?(:key), u.respond_to?(:key, true)
class PNE < NameError; private :name, :receiver; end
n = PNE.new("m")
p n.respond_to?(:name), n.respond_to?(:name, true), n.respond_to?(:receiver), n.respond_to?(:receiver, true)
# a sibling accessor the class did not touch stays public
class PSE < SystemExit; private :status; end
s = PSE.new(1)
p s.respond_to?(:status), s.respond_to?(:status, true), s.respond_to?(:success?)
