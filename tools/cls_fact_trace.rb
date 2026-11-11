# Loaded with `ruby -r` before a program: records, for every call of a
# Ruby-defined method, the call site and the receiver's class, and for every
# call of a method CRuby defines in C a receiver whose class is a builtin
# one, so tools/cls_fact_check.rb can hold spinel's class facts (#8530) and
# their builtin classes against what CRuby saw. One line per distinct
# observation, tab separated:
#   path  line  method  receiver  file-the-receiver's-class-was-defined-in  callee-file
# The receiver is the class name, or Class(X) for a class receiver. The
# callee's file tells a program method from CRuby's own Ruby-defined ones
# (`<internal:kernel>`'s #class, #tap, ...). A call a C method of CRuby
# makes back into the program (Array#== calling an element's #==) is
# recorded with the callee file prefixed by "callback:": its receiver is
# not the receiver of the call written on the line. The method is the name
# the call was written with (callee_id), not the one an alias names
# (method_id): the facts are keyed by the written name. A call of a C
# method is recorded with the callee "c_call" and the receiver's exact
# builtin class (Kernel#class, so a program's own `class` does not answer).
# CRuby runs `x.send(:m)` without a call of send, so a send's receiver is
# the one the call of m has, on the line of the send.
out = File.open(ENV.fetch("CLS_FACT_TRACE_OUT"), "a")
out.sync = true
seen = {}
# Kernel's own, so a program's #class or #is_a? (or a BasicObject) does not answer
k_class = Kernel.instance_method(:class)
k_is_a = Kernel.instance_method(:is_a?)
# Module's own, so a class method `name` of the program's does not answer
m_name = Module.instance_method(:name)
c_methods = {}
c_method = lambda do |label|
  c_methods.fetch(label) do
    c_methods[label] = begin
      m = label.match(/\A([A-Z][\w:]*)([#.])(.+)\z/)
      k = m && Object.const_get(m[1])
      um = m && (m[2] == "#" ? k.instance_method(m[3]) : k.method(m[3]))
      um ? um.source_location.nil? : false
    rescue StandardError
      false
    end
  end
end
TracePoint.new(:call) do |tp|
  loc = caller_locations(2, 1)&.first
  next unless loc
  path = loc.absolute_path || loc.path
  next unless path
  recv = tp.self
  mod = k_is_a.bind_call(recv, Module)
  name = m_name.bind_call(mod ? recv : k_class.bind_call(recv))
  next unless name
  shown = mod ? "Class(#{name})" : name
  key = [path, loc.lineno, tp.callee_id, shown, tp.path]
  next if seen[key]
  seen[key] = true
  src = (Object.const_source_location(name) rescue nil)&.first || ""
  callee = c_method.(loc.label) ? "callback:#{tp.path}" : tp.path
  out.puts [path, loc.lineno, tp.callee_id, shown, src, callee].join("\t")
end.enable
# the library files' calls are not the program's
skip = [RbConfig::CONFIG["rubylibdir"], RbConfig::CONFIG["rubyarchdir"], RbConfig::CONFIG["rubylibprefix"]].compact
skip += [Gem.default_dir, Gem.user_dir] if defined?(Gem)
# a class a file of the program defines (CRuby places Rational and Complex at <main>, line 0)
program_class = ->(n) { (f = (Object.const_source_location(n) rescue nil)&.first) && f != "<main>" }
# the builtin class (not Object) a class of the program's descends from, by name
builtin_root = Hash.new do |h, k|
  a = k.ancestors.find do |x|
    next false unless x.is_a?(Class) && x != k && x != Object && x != BasicObject
    xn = m_name.bind_call(x)
    xn && !program_class.(xn)
  end
  h[k] = a && m_name.bind_call(a)
end
TracePoint.new(:c_call) do |tp|
  path = tp.path
  next if !path || path.start_with?("<")
  path = File.expand_path(path)
  next if skip.any? { |d| path.start_with?(d) }
  # a call a C method makes of another is not the call written on the line
  caller = caller_locations(1, 1)&.first
  next if caller && c_method.(caller.label)
  recv = tp.self
  rclass = k_class.bind_call(recv)
  name = m_name.bind_call(rclass)
  next unless name
  key = [path, tp.lineno, tp.callee_id, name, :c]
  next if seen[key]
  seen[key] = true
  # a program's class: the :call trace has it, but its call of a C method it inherits from a builtin class
  # (MyA < Array, `rotate!`) is recorded too, with that builtin class as the seventh field
  if program_class.(name)
    root = builtin_root[rclass]
    out.puts [path, tp.lineno, tp.callee_id, name, "", "c_call", root].join("\t") if root
    next
  end
  out.puts [path, tp.lineno, tp.callee_id, name, "", "c_call"].join("\t")
end.enable
