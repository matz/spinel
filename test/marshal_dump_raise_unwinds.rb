# spinel: gc-stress
# Rescued Marshal.dump and Marshal.load errors unwind cleanly: temporary heap
# buffers (writer bytes, symbol table, object-link tables; reader tables) are
# freed on longjmp, GC roots are restored, and the raised exception reaches
# the caller intact.

def dump_payload(bad)
  {
    k1: "s1", k2: "s2", k3: "s3", k4: "s4", k5: "s5",
    k6: "s6", k7: "s7", k8: "s8", k9: "s9", k10: "s10",
    arr: [
      "a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8",
      "a9", "a10", "a11", "a12", "a13", "a14", "a15", "a16",
      [1, 2, 3], { nested: "value" }, bad
    ]
  }
end

bad_hash = Hash.new { |h, k| k }
bad_proc = -> { 1 }
bad_anon = Class.new.new
bad_sing = Object.new
def bad_sing.extra = 1

dump_cases = [bad_hash, bad_proc, bad_anon, bad_sing]
dump_counts = [0, 0, 0, 0]

25.times do
  dump_cases.each_with_index do |bad, idx|
    begin
      Marshal.dump(dump_payload(bad))
    rescue TypeError => e
      dump_counts[idx] += 1 if e.class == TypeError
    end
  end
end
p dump_counts

begin
  Marshal.dump(dump_payload(bad_hash))
rescue TypeError => e
  p e.message
end

begin
  Marshal.dump(dump_payload(bad_sing))
rescue TypeError => e
  p e.message
end

begin
  begin
    Marshal.dump(dump_payload(bad_hash))
  rescue TypeError => e
    raise e
  end
rescue => e2
  p e2.equal?(e), e2.class, e2.message
end

bad_loads = [
  "\x04\b[\x08:\x06a:\x06b\"\x20short".b,
  "\x04\b[\x0f:\x06a:\x06b:\x06c:\x06d:\x06e:\x06f:\x06g:\x06h:\x06io:\bZzz\x00".b,
  "\x04\b[\x17".b + ("\"\x06a".b * 17) + "o:\bZzz\x00".b,
  "\x04\b[\x07:\x06a\xfe".b
]
load_counts = [0, 0, 0, 0]

25.times do
  bad_loads.each_with_index do |data, idx|
    begin
      Marshal.load(data)
    rescue ArgumentError => e
      load_counts[idx] += 1 if e.class == ArgumentError
    end
  end
end
p load_counts

begin
  Marshal.load(bad_loads[0])
rescue ArgumentError => e
  p e.message
end

p Marshal.load(Marshal.dump({ ok: [1, "two", :three] }))
