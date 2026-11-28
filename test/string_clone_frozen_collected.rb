# A clone of a frozen String is a frozen String the collector frees once
# nothing holds it, as CRuby's is. The runtime gave the copy the 0xf1
# marker, which pins its slot for the rest of the run, so every clone of
# a frozen String, and every clone(freeze: true), stayed: a loop that made
# one a time grew without bound (130 MB for a million), and GC.stat could
# not show it, since a pinned String is in no count it keeps. The resident
# set is read before and after the churn; the bound is far above what
# either runtime holds and far below what the pinned copies took.
N = 300_000
LIMIT_KB = 12_000

def rss_kb
  if File.exist?("/proc/self/status")
    kb = 0
    File.read("/proc/self/status").each_line do |l|
      kb = l.split[1].to_i if l.start_with?("VmRSS:")
    end
    kb
  else
    `ps -o rss= -p #{Process.pid}`.strip.to_i
  end
end

def clone_frozen(base, n)
  t = 0
  i = 0
  while i < n
    c = (base + i.to_s).freeze.clone
    t += 1 if c.frozen?
    i += 1
  end
  t
end

def clone_freeze_true(base, n)
  t = 0
  i = 0
  while i < n
    c = (base + i.to_s).clone(freeze: true)
    t += 1 if c.frozen?
    i += 1
  end
  t
end

base = "abcdefghij"
clone_frozen(base, 10_000)
clone_freeze_true(base, 10_000)
GC.start

before = rss_kb
p clone_frozen(base, N)
GC.start
p rss_kb - before < LIMIT_KB

before = rss_kb
p clone_freeze_true(base, N)
GC.start
p rss_kb - before < LIMIT_KB

# the copies are still frozen, equal and distinct, and a mutable String's
# clone is still mutable
f = (base + "1").freeze
c = f.clone
p c.frozen?, c == f, c.equal?(f)
m = (base + "2").clone
m << "!"
p m.frozen?, m
p "lit".clone.frozen?
