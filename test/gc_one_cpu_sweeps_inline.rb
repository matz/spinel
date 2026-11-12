# spinel: linux -- only Linux holds a process to chosen CPUs (taskset), and how many it may use is the subject
# A process that may run on a single CPU starts no sweeper thread, and its
# collector sweeps under its own stop. A sweeper there cannot run beside the
# program, only instead of it: handing it the sweep costs a wake-up and the
# switches there and back at every collection, for work the collector does
# in the same time.
# SPINEL_GC_SWEEPERS=N still starts N, SPINEL_GC_CONC=0 still turns them off,
# and with two CPUs the sweep runs beside the program as before.
#
# The test runs itself as a child held to its first one or two allowed CPUs
# and reads what SPINEL_GC_PHASES reports there: the "concurrent sweep" line
# is printed only when a sweep ran beside the program. Each answer is the
# pair [the child computed what this process computes, it swept beside the
# program]. The child is given the collector's settings of its case alone,
# whatever this process was started with. CRuby has no such report, so there
# the second is given without looking, as it is where there is no taskset to
# hold the child with, and for the two-CPU cases where the process may not
# use two (one CPU in its mask, or a CPU quota from its cgroup of less than
# two). A child that does not exit cleanly, or prints anything but its sum,
# answers false for the first.
require "open3"

def churn
  ths = 4.times.map do |t|
    Thread.new do
      sum = 0
      20_000.times { |i| sum += [i, t].to_s.size + { i => t.to_s }.size }
      GC.start
      sum
    end
  end
  ths.sum(&:value)
end

# The CPUs of this process's affinity mask, from "0-3,8".
def allowed_cpus
  list = File.read("/proc/self/status")[/^Cpus_allowed_list:\s*(\S+)/, 1].to_s
  list.split(",").flat_map do |r|
    a, b = r.split("-")
    (a.to_i..(b || a).to_i).to_a
  end
end

# The CPUs a cgroup's quota leaves the process, rounded up as the runtime
# rounds them, or nil for no quota: cgroup v2's cpu.max ("200000 100000" is
# two, "max 100000" is none), else v1's cpu.cfs_quota_us over
# cpu.cfs_period_us (a quota of -1 is none).
def quota_cpus
  v2 = "/sys/fs/cgroup/cpu.max"
  if File.exist?(v2)
    quota, period = File.read(v2).split
    return nil if quota == "max"
    quota, period = quota.to_i, period.to_i
  else
    v1 = "/sys/fs/cgroup/cpu/cpu.cfs_"
    return nil unless File.exist?(v1 + "quota_us") && File.exist?(v1 + "period_us")
    quota, period = File.read(v1 + "quota_us").to_i, File.read(v1 + "period_us").to_i
  end
  return nil if quota <= 0 || period <= 0
  (quota + period - 1) / period
end

def taskset?
  ENV["PATH"].to_s.split(":").any? { |d| File.executable?(File.join(d, "taskset")) }
end

# What a child is told about its collector: these and nothing inherited.
SETTINGS = %w[SPINEL_WORKERS SPINEL_GC_SWEEPERS SPINEL_GC_CONC SPINEL_GC_OWNER SPINEL_GC_VERIFY
              SPINEL_GC_VERIFY_GEN SPINEL_GC_AGE SPINEL_GC_STRESS].freeze

def beside_program(cpus, settings, otherwise, expected)
  return [true, otherwise] unless cpus
  env = { "SPINEL_GC_PHASES" => "1" }
  SETTINGS.each { |k| env[k] = settings[k] }
  out, err, status = Open3.capture3(env, "taskset", "-c", cpus.join(","), $0, "child")
  [status.success? && out.strip == expected.to_s, err.include?("[gcph] concurrent sweep:")]
end

if ARGV[0] == "child"
  puts churn
else
  cpus = RUBY_ENGINE == "spinel" && taskset? ? allowed_cpus : []
  one = cpus.empty? ? nil : cpus.first(1)
  quota = quota_cpus
  two = cpus.size >= 2 && (quota.nil? || quota >= 2) ? cpus.first(2) : nil
  sum = churn
  p beside_program(one, {}, false, sum)
  p beside_program(one, { "SPINEL_WORKERS" => "1" }, false, sum)
  p beside_program(one, { "SPINEL_WORKERS" => "4" }, false, sum)
  p beside_program(one, { "SPINEL_GC_SWEEPERS" => "1" }, true, sum)
  p beside_program(one, { "SPINEL_GC_SWEEPERS" => "2", "SPINEL_WORKERS" => "1" }, true, sum)
  p beside_program(one, { "SPINEL_GC_SWEEPERS" => "1", "SPINEL_GC_CONC" => "0" }, false, sum)
  p beside_program(two, {}, true, sum)
  p beside_program(two, { "SPINEL_WORKERS" => "1" }, true, sum)
  p beside_program(two, { "SPINEL_GC_CONC" => "0" }, false, sum)
end
