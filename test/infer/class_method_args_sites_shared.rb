# Three calls with arguments of a class method on the Class a run-time
# const_get answers -- a local, a literal and a call as the argument --
# share one dispatch function over the classes (#12511), not a copy at
# each site. (Past 64 classes a local or a literal argument is hoisted
# too, so the arms read only temps.)
class K1; def self.v(a) = a + 1; end
class K2; def self.v(a) = a + 2; end
class K3; def self.v(a) = a + 3; end
class K4; def self.v(a) = a + 4; end
class K5; def self.v(a) = a + 5; end
class K6; def self.v(a) = a + 6; end
class K7; def self.v(a) = a + 7; end
class K8; def self.v(a) = a + 8; end
class K9; def self.v(a) = a + 9; end
class K10; def self.v(a) = a + 10; end
class K11; def self.v(a) = a + 11; end
class K12; def self.v(a) = a + 12; end
class K13; def self.v(a) = a + 13; end
class K14; def self.v(a) = a + 14; end
class K15; def self.v(a) = a + 15; end
class K16; def self.v(a) = a + 16; end
class K17; def self.v(a) = a + 17; end
class K18; def self.v(a) = a + 18; end
class K19; def self.v(a) = a + 19; end
class K20; def self.v(a) = a + 20; end
class K21; def self.v(a) = a + 21; end
class K22; def self.v(a) = a + 22; end
class K23; def self.v(a) = a + 23; end
class K24; def self.v(a) = a + 24; end
class K25; def self.v(a) = a + 25; end
class K26; def self.v(a) = a + 26; end
class K27; def self.v(a) = a + 27; end
class K28; def self.v(a) = a + 28; end
class K29; def self.v(a) = a + 29; end
class K30; def self.v(a) = a + 30; end
class K31; def self.v(a) = a + 31; end
class K32; def self.v(a) = a + 32; end
class K33; def self.v(a) = a + 33; end
class K34; def self.v(a) = a + 34; end
class K35; def self.v(a) = a + 35; end
class K36; def self.v(a) = a + 36; end
class K37; def self.v(a) = a + 37; end
class K38; def self.v(a) = a + 38; end
class K39; def self.v(a) = a + 39; end
class K40; def self.v(a) = a + 40; end
class K41; def self.v(a) = a + 41; end
class K42; def self.v(a) = a + 42; end
class K43; def self.v(a) = a + 43; end
class K44; def self.v(a) = a + 44; end
class K45; def self.v(a) = a + 45; end
class K46; def self.v(a) = a + 46; end
class K47; def self.v(a) = a + 47; end
class K48; def self.v(a) = a + 48; end
class K49; def self.v(a) = a + 49; end
class K50; def self.v(a) = a + 50; end
class K51; def self.v(a) = a + 51; end
class K52; def self.v(a) = a + 52; end
class K53; def self.v(a) = a + 53; end
class K54; def self.v(a) = a + 54; end
class K55; def self.v(a) = a + 55; end
class K56; def self.v(a) = a + 56; end
class K57; def self.v(a) = a + 57; end
class K58; def self.v(a) = a + 58; end
class K59; def self.v(a) = a + 59; end
class K60; def self.v(a) = a + 60; end
class K61; def self.v(a) = a + 61; end
class K62; def self.v(a) = a + 62; end
class K63; def self.v(a) = a + 63; end
class K64; def self.v(a) = a + 64; end
class K65; def self.v(a) = a + 65; end
class K66; def self.v(a) = a + 66; end
class K67; def self.v(a) = a + 67; end
class K68; def self.v(a) = a + 68; end
class K69; def self.v(a) = a + 69; end
class K70; def self.v(a) = a + 70; end
names = %w[K7 K12]
def two = 2
n = 5
p Object.const_get(names[0]).v(n)
p Object.const_get(names[1]).v(1)
p Object.const_get(names[0]).v(two)
