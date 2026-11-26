# The match family runs its receiver before its pattern, as CRuby
# evaluates them, where the pattern runs code.
$t = []; (($t << 1; +"q") =~ ($t << 2; /b/)); p $t
$t = []; r = (($t << 1; +"qb") =~ ($t << 2; /b/)); p [r, $t]
$t = []; p((($t << 1; "qb".dup) =~ ($t << 2; /b/)), $t)
$t = []; s = "xb"; p((($t << 1; s) =~ ($t << 2; Regexp.new("b"))), $t)
$t = []; p((($t << 1; /b/) =~ ($t << 2; "ab")), $t)
$t = []; x = nil; p((($t << 1; x) =~ ($t << 2; /b/)), $t)
$t = []; p((($t << 1; :sym) =~ ($t << 2; /y/)), $t)
$t = []; p((($t << 1; "ab") !~ ($t << 2; /b/)), $t)
$t = []; p((($t << 1; "ab").match?(($t << 2; /b/))), $t)
$t = []; p((($t << 1; "ab").match(($t << 2; /b/)))[0], $t)
$t = []; s = "ab"; p((s =~ (s = "zz"; /b/)), $t)
y = ARGV.empty? ? 5 : "a"
begin
  ($t << 1; y) =~ ($t << 2; /b/)
rescue NoMethodError => e
  p e.message
end
def pat = ($t << :pat; /a/)
$t = []; p((($t << :recv; "ca") =~ pat), $t)
