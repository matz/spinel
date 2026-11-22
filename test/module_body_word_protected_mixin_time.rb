# A protected method's caller is open where its self is kind_of? the module:
# a class that prepends the module only in a later reopening is not before
# that reopening runs, and an object that extends the module may be, where a
# method body's word made the module's method protected.
module F; def q = :fq; protected :q; end
class FA; include F; end
class FC; def via(o) = (o.q rescue $!.class); end
p FC.new.via(FA.new)
class FC; prepend F; end
module E; def q = :eq; def self.prot = (protected :q; nil); end
class EA; include E; end
class EC; def via(o) = (o.q rescue $!.class); end
E.prot
ec = EC.new; ec.extend(E)
p ec.via(EA.new)
