# m::X through a value reads the constant in the order CRuby's ancestors
# list gives: each module sits once, where it was first put, and a
# prepend inside an included module keeps CRuby's rules.

# K includes M1, M2, then M3, which includes M1 again. M1 stays where it
# was first put, so M2's Y comes first.
module M1; Y = 10; end
module M2; Y = 20; end
module M3; include M1; end
class K; include M1; include M2; include M3; end
[K].each { |k| p k::Y }

# C2 prepends M3, and M3 includes M1, which C1 has through M3. A prepend
# puts M1 in again, ahead of C1.
module N1; X = 1; end
module N3; include N1; end
class C1; X = 100; include N3; end
class C2 < C1; prepend N3; end
[C1, C2].each { |k| p k::X }

# C4 includes P3, which prepends P1. C4's superclass already includes P1,
# so P1 is not put in again and P3's own X comes first.
module P1; X = 1; end
module P3; X = 3; prepend P1; end
class D2; include P1; end
class D4 < D2; include P3; end
[D4].each { |k| p k::X }

# A module's constants come after the modules it prepends.
module Q1; Z = :q1; end
module Q2; Z = :q2; prepend Q1; end
class E1; include Q2; end
[E1].each { |k| p k::Z }

# A later prepend comes ahead of an earlier one.
module R1; W = :r1; end
module R2; W = :r2; end
class F1; prepend R1; prepend R2; end
[F1].each { |k| p k::W }

# A superclass's prepend comes ahead of the superclass's own constant.
module S1; V = :s1; end
class G1; V = :g1; prepend S1; end
class G2 < G1; end
[G1, G2].each { |k| p k::V }


# A superclass's prepend sits after the superclass's head, so a module an
# include brings again there does not move the insertion point.
module U1; X = :u1; end
module U2; X = :u2; end
module U3; include U2; include U1; end
class I1; prepend U1; end
class I2 < I1; include U3; end
[I2].each { |k| p k::X }

# A long superclass chain still reaches the top class's constant.
class L0; X = :l0; end
class L1 < L0; end; class L2 < L1; end; class L3 < L2; end; class L4 < L3; end
class L5 < L4; end; class L6 < L5; end; class L7 < L6; end; class L8 < L7; end
class L9 < L8; end; class L10 < L9; end; class L11 < L10; end; class L12 < L11; end
class L13 < L12; end; class L14 < L13; end; class L15 < L14; end; class L16 < L15; end
class L17 < L16; end; class L18 < L17; end; class L19 < L18; end
[L19].each { |c| p c::X }
