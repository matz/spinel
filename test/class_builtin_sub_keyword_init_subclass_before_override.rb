# A subclass of a keyword-init Struct class inspects with the suffix too, as
# the class it inherits from does, and a call of its inspect that runs before
# the program's class method of its own answers so (see
# class_builtin_sub_keyword_init_before_override.rb). The Struct classes
# whose keyword_init is unset or false have no suffix, at any depth.

class KB < Struct.new(:a, keyword_init: true); end
class KS < KB; end
class KT < KS; end
class PB < Struct.new(:a); end
class PS < PB; end
class FB < Struct.new(:a, keyword_init: false); end
class FS < FB; end
KC = Struct.new(:a, keyword_init: true)
class KCS < KC; end
class DB < Data.define(:a); end
class DS < DB; end

def early = KT.inspect
def early_of(k) = k.inspect
def texts = [KB.inspect, KS.inspect, PB.inspect, PS.inspect, FB.inspect, FS.inspect, DB.inspect, DS.inspect]
p early, early_of(KS), KS.inspect, texts
p early_of(KC), early_of(KCS), KC.inspect, KCS.inspect
puts KS.inspect, KT.inspect

class KT
  def self.inspect = "kt"
end
p early, early_of(KS), early_of(KT), KS.inspect, texts
