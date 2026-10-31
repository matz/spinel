# Eight modules included by one reopening of a class, each of a method the
# class does not have: every copy keeps its module method's keyword
# parameter. Which copy came out wrong (`a3(k: 3)` answering `{k: 3}`)
# depended on where the scope table had been moved to.
module M0; def a0(k: 1) = k; end
module M1; def a1(k: 1) = k; end
module M2; def a2(k: 1) = k; end
module M3; def a3(k: 1) = k; end
module M4; def a4(k: 1) = k; end
module M5; def a5(k: 1) = k; end
module M6; def a6(k: 1) = k; end
module M7; def a7(k: 1) = k; end
class C; def z = 0; end
class C
  include M0
  include M1
  include M2
  include M3
  include M4
  include M5
  include M6
  include M7
end
c = C.new
p [c.a0(k: 0), c.a1(k: 1), c.a2(k: 2), c.a3(k: 3), c.a4(k: 4), c.a5(k: 5), c.a6(k: 6), c.a7(k: 7)]
p [c.a0, c.a7]
