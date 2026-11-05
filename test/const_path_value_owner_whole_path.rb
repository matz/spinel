# `m::Foo` with m a value reads the Foo of the class or module m is, by
# that holder's whole path: a top-level M is not A::M, and a top-level K
# without a Foo raises NameError, not the Foo of X::K.
module A; module M; Foo = 1; end; end
module B; module M; Foo = 2; end; end
module M; Foo = 3; end
module A::C; Foo = 4; end
module C; Foo = 5; end
module X; class K; Foo = 7; end; end
class K; end
def look(m) = m::Foo
[A::M, B::M, M, A::C, C, X::K].each { |m| p look(m) }
begin
  p look(K)
rescue NameError => e
  puts e.message
end
p A::M::Foo, M::Foo
# a header written through an alias keeps its constants
AliasA = A
module AliasA::M2; Foo = 9; end
class AliasA::K2; Foo = 8; end
[A::M2, A::K2].each { |m| p look(m) }
