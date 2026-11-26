# A splat spreads its Array where it stands: a later argument that pushes
# onto the same Array (an ivar, a global, inline or in a method it calls)
# does not reach the splat's elements, as in CRuby.
def f_star(*r) = r.inspect
def im(k) = ($t << k; @a << 9; 2)
def gm(k) = ($t << k; $a << 9; 2)

$t = []; @a = [1, 2, 3]
p f_star(*@a, ($t << 2; @a << 9; 2)), @a
$t = []; @a = [1, 2, 3]
p f_star(*@a, im(2)), @a, $t
$t = []; $a = [1, 2, 3]
p f_star(*$a, gm(2)), $a, $t
a = [1, 2, 3]
p f_star(*a, (a << 9; 2)), a
@w = %w[x y]
p f_star(*@w, (@w << "z"; "w")), @w
@f = [1.5]
p f_star(*@f, (@f << 2.5; 0.5)), @f
