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
