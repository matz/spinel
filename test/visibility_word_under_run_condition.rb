# A visibility word that stands under a condition the run decides changes a plain def's visibility only where it runs.
# The word's statement is placed in the class body, but whether it runs is the program's: the call, respond_to?, the
# lists and the method_defined? kin read the name's visibility where the run leaves it, as CRuby does.
NO = ENV.key?("SPINEL_NO_SUCH_VARIABLE_FOR_THIS_TEST")   # false
YES = !NO                                                  # true

# what an instance answers: the call, respond_to?, public_send and send
def show(c)
  call = begin; c.tail; rescue NoMethodError => e; e.message[/\A(private|protected) method/] || e.class; end
  sent = begin; c.public_send(:tail); rescue NoMethodError; :refused; end
  [call, c.respond_to?(:tail), sent, c.send(:tail)]
end

# the word runs: private, public and protected
class PrivYes; def tail = :tail; private :tail if YES; end
class PubYes; private def tail = :tail; public :tail if YES; end
class ProtYes; def tail = :tail; protected :tail if YES; end
# the word does not run
class PrivNo; def tail = :tail; private :tail if NO; end
class PubNo; private def tail = :tail; public :tail if NO; end
class ProtNo; def tail = :tail; protected :tail if NO; end
p [PrivYes, show(PrivYes.new), PrivYes.public_method_defined?(:tail), PrivYes.private_method_defined?(:tail), PrivYes.protected_method_defined?(:tail),
   PrivYes.public_instance_methods(false), PrivYes.private_instance_methods(false), PrivYes.protected_instance_methods(false)]
p [PubYes, show(PubYes.new), PubYes.public_method_defined?(:tail), PubYes.private_method_defined?(:tail), PubYes.protected_method_defined?(:tail),
   PubYes.public_instance_methods(false), PubYes.private_instance_methods(false), PubYes.protected_instance_methods(false)]
p [ProtYes, show(ProtYes.new), ProtYes.public_method_defined?(:tail), ProtYes.private_method_defined?(:tail), ProtYes.protected_method_defined?(:tail),
   ProtYes.public_instance_methods(false), ProtYes.private_instance_methods(false), ProtYes.protected_instance_methods(false)]
p [PrivNo, show(PrivNo.new), PrivNo.public_method_defined?(:tail), PrivNo.private_method_defined?(:tail), PrivNo.protected_method_defined?(:tail),
   PrivNo.public_instance_methods(false), PrivNo.private_instance_methods(false), PrivNo.protected_instance_methods(false)]
p [PubNo, show(PubNo.new), PubNo.public_method_defined?(:tail), PubNo.private_method_defined?(:tail), PubNo.protected_method_defined?(:tail),
   PubNo.public_instance_methods(false), PubNo.private_instance_methods(false), PubNo.protected_instance_methods(false)]
p [ProtNo, show(ProtNo.new), ProtNo.public_method_defined?(:tail), ProtNo.private_method_defined?(:tail), ProtNo.protected_method_defined?(:tail),
   ProtNo.public_instance_methods(false), ProtNo.private_instance_methods(false), ProtNo.protected_instance_methods(false)]

# the other forms of the condition
class Unless; def tail = :tail; private :tail unless NO; end
class UnlessNo; def tail = :tail; private :tail unless YES; end
class Ternary; def tail = :tail; NO ? private(:tail) : nil; end
class AndYes; def tail = :tail; YES && private(:tail); end
class AndNo; def tail = :tail; NO && private(:tail); end
class OrNo; def tail = :tail; NO || private(:tail); end
class Sent; def tail = :tail; send(:private, :tail) if NO; end
class ArrayArg; def tail = :tail; private [:tail] if NO; end
class Nested; def tail = :tail; if YES; if NO; private :tail; end; end; end
class NestedYes; def tail = :tail; if YES; if YES; begin; private :tail; end; end; end; end
p [Unless, show(Unless.new), Unless.public_method_defined?(:tail), Unless.private_method_defined?(:tail), Unless.protected_method_defined?(:tail),
   Unless.public_instance_methods(false), Unless.private_instance_methods(false), Unless.protected_instance_methods(false)]
p [UnlessNo, show(UnlessNo.new), UnlessNo.public_method_defined?(:tail), UnlessNo.private_method_defined?(:tail), UnlessNo.protected_method_defined?(:tail),
   UnlessNo.public_instance_methods(false), UnlessNo.private_instance_methods(false), UnlessNo.protected_instance_methods(false)]
p [Ternary, show(Ternary.new), Ternary.public_method_defined?(:tail), Ternary.private_method_defined?(:tail), Ternary.protected_method_defined?(:tail),
   Ternary.public_instance_methods(false), Ternary.private_instance_methods(false), Ternary.protected_instance_methods(false)]
p [AndYes, show(AndYes.new), AndYes.public_method_defined?(:tail), AndYes.private_method_defined?(:tail), AndYes.protected_method_defined?(:tail),
   AndYes.public_instance_methods(false), AndYes.private_instance_methods(false), AndYes.protected_instance_methods(false)]
p [AndNo, show(AndNo.new), AndNo.public_method_defined?(:tail), AndNo.private_method_defined?(:tail), AndNo.protected_method_defined?(:tail),
   AndNo.public_instance_methods(false), AndNo.private_instance_methods(false), AndNo.protected_instance_methods(false)]
p [OrNo, show(OrNo.new), OrNo.public_method_defined?(:tail), OrNo.private_method_defined?(:tail), OrNo.protected_method_defined?(:tail),
   OrNo.public_instance_methods(false), OrNo.private_instance_methods(false), OrNo.protected_instance_methods(false)]
p [Sent, show(Sent.new), Sent.public_method_defined?(:tail), Sent.private_method_defined?(:tail), Sent.protected_method_defined?(:tail),
   Sent.public_instance_methods(false), Sent.private_instance_methods(false), Sent.protected_instance_methods(false)]
p [ArrayArg, show(ArrayArg.new), ArrayArg.public_method_defined?(:tail), ArrayArg.private_method_defined?(:tail), ArrayArg.protected_method_defined?(:tail),
   ArrayArg.public_instance_methods(false), ArrayArg.private_instance_methods(false), ArrayArg.protected_instance_methods(false)]
p [Nested, show(Nested.new), Nested.public_method_defined?(:tail), Nested.private_method_defined?(:tail), Nested.protected_method_defined?(:tail),
   Nested.public_instance_methods(false), Nested.private_instance_methods(false), Nested.protected_instance_methods(false)]
p [NestedYes, show(NestedYes.new), NestedYes.public_method_defined?(:tail), NestedYes.private_method_defined?(:tail), NestedYes.protected_method_defined?(:tail),
   NestedYes.public_instance_methods(false), NestedYes.private_instance_methods(false), NestedYes.protected_instance_methods(false)]

# two words on one name, the run decides each
class Two; def tail = :tail; private :tail if YES; public :tail if NO; end
class TwoLater; def tail = :tail; private :tail if NO; protected :tail if YES; end
class Flip; def tail = :tail; private :tail if NO; public :tail; private :tail if NO; end
p [Two, show(Two.new), Two.public_method_defined?(:tail), Two.private_method_defined?(:tail), Two.protected_method_defined?(:tail),
   Two.public_instance_methods(false), Two.private_instance_methods(false), Two.protected_instance_methods(false)]
p [TwoLater, show(TwoLater.new), TwoLater.public_method_defined?(:tail), TwoLater.private_method_defined?(:tail), TwoLater.protected_method_defined?(:tail),
   TwoLater.public_instance_methods(false), TwoLater.private_instance_methods(false), TwoLater.protected_instance_methods(false)]
p [Flip, show(Flip.new), Flip.public_method_defined?(:tail), Flip.private_method_defined?(:tail), Flip.protected_method_defined?(:tail),
   Flip.public_instance_methods(false), Flip.private_instance_methods(false), Flip.protected_instance_methods(false)]

# a subclass and an including class see the same
class Base; def tail = :tail; private :tail if YES; end
class Mid < Base; end
class Leaf < Mid; end
module Mixin; def tail = :tail; private :tail if NO; end
class Includer; include Mixin; end
p [Leaf, show(Leaf.new), Leaf.public_method_defined?(:tail), Leaf.private_method_defined?(:tail), Leaf.protected_method_defined?(:tail),
   Leaf.public_instance_methods(false), Leaf.private_instance_methods(false), Leaf.protected_instance_methods(false)]
p [Includer, show(Includer.new), Includer.public_method_defined?(:tail), Includer.private_method_defined?(:tail), Includer.protected_method_defined?(:tail),
   Includer.public_instance_methods(false), Includer.private_instance_methods(false), Includer.protected_instance_methods(false)]

# a call before the word and after it
class Between
  def tail = :tail
  BEFORE = (new.tail rescue $!.class)
  private :tail if YES
  AFTER = (new.tail rescue $!.class)
end
p [Between::BEFORE, Between::AFTER, show(Between.new), Between.private_instance_methods(false)]
