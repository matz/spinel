# respond_to? of a record class's own class methods (members, [], a
# Struct's keyword_init?) answers true on the class, a subclass and an
# anonymous Struct.new class, before the program's def of one as after
# it; a Data class has no keyword_init?.
R = Struct.new(:r, keyword_init: true)
E = Data.define(:e)
class T < Struct.new(:t); end
p R.respond_to?(:keyword_init?), R.respond_to?(:[]), R.respond_to?(:members)
p E.respond_to?(:keyword_init?), E.respond_to?(:[]), E.respond_to?(:members)
p T.respond_to?(:members), Struct.new(:z).respond_to?(:members)
p R.respond_to?(:nope), E.respond_to?(:with)
def R.keyword_init? = :r
p R.respond_to?(:keyword_init?), R.keyword_init?
