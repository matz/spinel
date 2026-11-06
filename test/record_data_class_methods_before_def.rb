# keyword_init? is a Struct class's own, not a Data class's: before the
# program's def, a Data class has none.
E = Data.define(:a)
def fe = (E.keyword_init? rescue $!.class)
p fe
def E.keyword_init? = :mine
p fe
class F < Data.define(:a); end
def ff = (F.keyword_init? rescue $!.class)
p ff
def F.keyword_init? = :mine
p ff
K = Struct.new(:a)
def fk = K.keyword_init?
p fk
def K.keyword_init? = :mine
p fk
