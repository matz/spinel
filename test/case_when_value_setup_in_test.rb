# A when value's own set-up (an Array it builds, a call's temporaries)
# runs where its test does: after the values before it, and only when they
# do not match. A splat's Array and a Range's ends are built there too.

$log = []
def fss = ($log << :fss; "s")
def fs = ($log << :fs; :s)
def f3 = ($log << :f3; 3)

case :s
when fs then p :one
when *[fss] then p :two
end
p $log

$log = []
case :t
when fs then p :one
when *[fss] then p :two
else p :none
end
p $log

$log = []
case 3
when f3 then p :one
when [fss.size].first..9 then p :two
end
p $log

$log = []
case 5
when f3 then p :one
when [fss.size].first..9 then p :two
end
p $log

$log = []
x = case :s
    when fs then :one
    when *[fss] then :two
    end
p x, $log

$log = []
y = case 5
    when f3 then :one
    when [fss.size].first..9 then :two
    end
p y, $log

# a value of another family than the subject's (a String against an
# Integer) never matches, but still runs in its turn, as CRuby evaluates it
def f2 = ($log << :f2; 2)
$log = []
case 2
when 1 then p :one
when f2 then p :two
when [fss].first then p :three
end
p $log

$log = []
case 3
when 1 then p :one
when f2 then p :two
when [fss].first then p :three
end
p $log

$log = []
z = case 3
    when f2 then :two
    when [fss].first then :three
    end
p z, $log
