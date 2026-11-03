# A backtick answers the child's WHOLE output: it was one fread of 4095 bytes.
p `seq 1 5000`.lines.size
p `printf 'a\\0b'`.bytes
p `exit 3`
p $?.to_i >> 8

# A backtick, a Kernel#system or a Process.waitpid2 in a green thread waited
# for the child in the kernel on its OS worker. A worker in a syscall never
# reaches a safepoint, so main's next collection waited for the child too
# (#4528). Main collects many times while the children run; the threads are
# still alive when it is done. The children run until main writes the file
# `go`, after its collections, so how long main takes does not matter (a busy
# macOS runner took over 2 s, past a child's fixed sleep); a collection that
# waited for them would never finish.
go = "#{ENV.fetch("TMPDIR", "/tmp")}/spinel_backtick_gc_#{Process.pid}"
wait = "while [ ! -e '#{go}' ]; do sleep 0.05; done"
slow = Thread.new { `#{wait}; echo slow` }
sys = Thread.new { system(wait) }
sleep 0.1
20.times { garbage = Array.new(100) { |i| "s#{i}" }; GC.start }
p slow.alive?, sys.alive?
File.write(go, "")
p slow.value
p sys.value
File.delete(go)
