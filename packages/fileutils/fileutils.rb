# Spinel bundled `fileutils` -- plain Ruby over Dir and File.
#
# Nothing here is carried C. Every method is a walk and a path calculation on
# primitives spinel already answers (Dir.mkdir / .rmdir / .entries / .exist?,
# File.directory? / .symlink? / .delete / .rename / .utime, IO.copy_stream),
# which is why this is a .rb and not a binding.
#
# What is absent is absent the way a subset is:
#
# * `verbose:` prints the equivalent shell command to $stdout before the
#   operation, spelled as CRuby spells it, and `noop:` prints (when verbose)
#   and does nothing else. Both are per call; there is no FileUtils::Verbose
#   / ::NoWrite / ::DryRun module to include.
# * no `preserve:` on cp/mv, no `dereference_root:`, no `secure:` on rm_r
# * no ln_sf, no chown, no cp_lr, and no owner: or group: on install
#
# Every method takes a single path or a list of them, as CRuby's do, and
# returns what CRuby's return (mkdir_p and friends answer the list they were
# given; rm_rf answers it too).
module FileUtils
  module_function

  # CRuby accepts a String, a Pathname, or an Array of either. `to_s` covers
  # the Pathname case without naming the class, which would drag the pathname
  # package into every program that requires this one.
  def fu_list(arg)
    arg.is_a?(Array) ? arg.map { |x| x.to_s } : [arg.to_s]
  end

  # `verbose: true` reports the shell command CRuby's FileUtils reports,
  # before the operation runs (and instead of it under `noop:`). CRuby sends
  # it to $stdout (fileutils.rb's own fu_output_message defaults there), not
  # to $stderr, so a program that pipes its output sees these lines in it.
  def fu_output_message(msg)
    puts msg
    nil
  end

  def fu_mode_to_s(mode)
    mode.is_a?(Integer) ? sprintf("%o", mode) : mode.to_s
  end

  # A String mode is chmod(1)'s symbolic form, which reads the file's mode, so
  # each path gets its own number.
  def fu_mode(mode, path)
    mode.is_a?(String) ? fu_symbolic_mode(mode, path) : mode
  end

  def fu_symbolic_mode(mode, path)
    st = File.stat(path)
    current = st.mode & 0o7777
    mode.split(",").each do |clause|
      # "u+x-w" is ["u", "+", "x", "-", "w"], the who and then op, perm pairs
      parts = clause.split(/([=+-])/)
      raise ArgumentError, "invalid file mode: #{mode}" if parts.length < 2
      who = fu_who_mask(parts[0].empty? ? "a" : parts[0])
      (1...parts.length).step(2) do |i|
        op = parts[i]
        apply = op == "="
        bits = 0
        perm = i + 1 < parts.length ? parts[i + 1] : ""
        perm.each_char do |chr|
          case chr
          when "r" then bits |= 0o444
          when "w" then bits |= 0o222
          when "x" then bits |= 0o111
          when "X" then bits |= 0o111 if st.directory?
          when "s" then bits |= 0o6000
          when "t" then bits |= 0o1000
          when "u", "g", "o"
            # `g=u`: the bits another class has now, moved to this one's place
            current = fu_apply_mask(current, who, op, bits) if bits != 0
            apply = false
            from = fu_who_mask(chr)
            bits = (current & from) / (from & 0o111) * (who & 0o111)
          else
            raise ArgumentError, "invalid 'perm' symbol in file mode: #{chr}"
          end
        end
        current = fu_apply_mask(current, who, op, bits) if bits != 0 || apply
      end
    end
    current
  end

  def fu_who_mask(target)
    mask = 0
    target.each_char do |chr|
      case chr
      when "u" then mask |= 0o4700
      when "g" then mask |= 0o2070
      when "o" then mask |= 0o1007
      when "a" then mask |= 0o7777
      else raise ArgumentError, "invalid 'who' symbol in file mode: #{chr}"
      end
    end
    mask
  end

  def fu_apply_mask(mode, who, op, bits)
    case op
    when "=" then (mode & ~who) | (who & bits)
    when "+" then mode | (who & bits)
    else mode & ~(who & bits)
    end
  end

  # mkdir_p makes every missing component, and is NOT an error when the
  # directory is already there -- that is the whole difference from Dir.mkdir
  # and the reason nearly every caller wants this one.
  def mkdir_p(list, mode: nil, verbose: nil, noop: nil)
    fu_output_message("mkdir -p #{mode ? sprintf('-m %03o ', mode) : ''}#{fu_list(list).join(' ')}") if verbose
    return fu_list(list) if noop
    fu_list(list).each do |path|
      next if path.empty?
      # Walk down from the root building each missing component. Splitting on
      # "/" keeps a leading "/" as an empty first element, which is what makes
      # an absolute path rebuild as absolute.
      parts = path.split("/")
      acc = path.start_with?("/") ? "" : nil
      parts.each do |part|
        next if part.empty?
        acc = acc.nil? ? part : "#{acc}/#{part}"
        next if Dir.exist?(acc)
        # A racing process may have created it between the test and the call,
        # and a plain file sitting in the way is an error CRuby also raises.
        begin
          Dir.mkdir(acc)
          File.chmod(mode, acc) if mode
        rescue SystemCallError
          raise unless Dir.exist?(acc)
        end
      end
    end
    fu_list(list)
  end

  def makedirs(list, mode: nil, verbose: nil, noop: nil)
    mkdir_p(list, mode: mode, verbose: verbose, noop: noop)
  end

  def mkpath(list, mode: nil, verbose: nil, noop: nil)
    mkdir_p(list, mode: mode, verbose: verbose, noop: noop)
  end

  def mkdir(list, mode: nil, verbose: nil, noop: nil)
    fu_output_message("mkdir #{mode ? sprintf('-m %03o ', mode) : ''}#{fu_list(list).join(' ')}") if verbose
    return fu_list(list) if noop
    fu_list(list).each do |path|
      Dir.mkdir(path)
      File.chmod(mode, path) if mode
    end
    fu_list(list)
  end

  def rmdir(list, parents: nil, verbose: nil, noop: nil)
    fu_output_message("rmdir #{parents ? '-p ' : ''}#{fu_list(list).join(' ')}") if verbose
    return fu_list(list) if noop
    fu_list(list).each { |path| Dir.rmdir(path) }
    fu_list(list)
  end

  # rm removes files, never directories: a directory here is an error, as it
  # is in CRuby. rm_f swallows every error, which is what `force: true` means.
  def rm(list, force: nil, verbose: nil, noop: nil)
    fu_output_message("rm#{force ? ' -f' : ''} #{fu_list(list).join(' ')}") if verbose
    return fu_list(list) if noop
    fu_list(list).each do |path|
      begin
        File.delete(path)
      rescue StandardError
        raise unless force
      end
    end
    fu_list(list)
  end

  def rm_f(list, verbose: nil, noop: nil)
    rm(list, force: true, verbose: verbose, noop: noop)
  end

  def remove(list, force: nil, verbose: nil, noop: nil)
    rm(list, force: force, verbose: verbose, noop: noop)
  end

  def remove_file(path, force = false)
    rm([path.to_s], force: force)
    nil
  end

  # rm_r walks a tree bottom-up. A SYMLINK to a directory is removed as a
  # link, not followed -- following it would delete a tree outside the one
  # named, which is the accident this check exists to prevent.
  def rm_r(list, force: nil, verbose: nil, noop: nil, secure: nil)
    fu_output_message("rm -r#{force ? 'f' : ''} #{fu_list(list).join(' ')}") if verbose
    return fu_list(list) if noop
    fu_list(list).each { |path| fu_rm_tree(path, force ? true : false) }
    fu_list(list)
  end

  def rm_rf(list, verbose: nil, noop: nil, secure: nil)
    rm_r(list, force: true, verbose: verbose, noop: noop)
  end

  def rmtree(list, verbose: nil, noop: nil, secure: nil)
    rm_r(list, force: true, verbose: verbose, noop: noop)
  end

  def remove_entry(path, force = false)
    fu_rm_tree(path.to_s, force ? true : false)
    nil
  end

  def remove_entry_secure(path, force = false)
    fu_rm_tree(path.to_s, force ? true : false)
    nil
  end

  def fu_rm_tree(path, force)
    if File.symlink?(path) || !File.directory?(path)
      begin
        File.delete(path)
      rescue StandardError
        raise unless force
        return nil
      end
      return nil
    end
    entries = begin
      Dir.entries(path)
    rescue StandardError
      raise unless force
      return nil
    end
    entries.each do |e|
      next if e == "." || e == ".."
      fu_rm_tree("#{path}/#{e}", force)
    end
    begin
      Dir.rmdir(path)
    rescue StandardError
      raise unless force
    end
    nil
  end

  # cp copies file contents. A destination that is an existing DIRECTORY takes
  # the source's basename inside it, which is what makes `cp(files, dir)` work.
  def cp(src, dest, preserve: nil, verbose: nil, noop: nil)
    fu_output_message("cp#{preserve ? ' -p' : ''} #{(fu_list(src) + [dest.to_s]).join(' ')}") if verbose
    return nil if noop
    fu_list(src).each { |s| fu_copy_file(s, fu_dest_path(s, dest.to_s)) }
    nil
  end

  def copy(src, dest, preserve: nil, verbose: nil, noop: nil)
    cp(src, dest, preserve: preserve, verbose: verbose, noop: noop)
  end

  def cp_r(src, dest, preserve: nil, verbose: nil, noop: nil, dereference_root: nil, remove_destination: nil)
    fu_output_message("cp -r#{preserve ? 'p' : ''} #{(fu_list(src) + [dest.to_s]).join(' ')}") if verbose
    return nil if noop
    fu_list(src).each { |s| fu_copy_tree(s, fu_dest_path(s, dest.to_s)) }
    nil
  end

  # copy_file copies one file's contents to dest, which takes the source's
  # mode when it is created; preserve also copies the times and the mode.
  # Symbolic links are always followed.
  def copy_file(src, dest, preserve = false, dereference = true)
    st = File.stat(src.to_s)
    File.open(src.to_s, "rb") do |s|
      File.open(dest.to_s, "wb", st.mode) { |f| IO.copy_stream(s, f) }
    end
    if preserve
      File.utime(st.atime, st.mtime, dest.to_s)
      File.chmod(st.mode & 0o7777, dest.to_s)
    end
    nil
  end

  def fu_dest_path(src, dest)
    Dir.exist?(dest) ? "#{dest}/#{File.basename(src)}" : dest
  end

  def fu_copy_file(src, dest)
    IO.copy_stream(src, dest)
    nil
  end

  def fu_copy_tree(src, dest)
    if File.directory?(src) && !File.symlink?(src)
      Dir.mkdir(dest) unless Dir.exist?(dest)
      Dir.entries(src).each do |e|
        next if e == "." || e == ".."
        fu_copy_tree("#{src}/#{e}", "#{dest}/#{e}")
      end
    else
      fu_copy_file(src, dest)
    end
    nil
  end

  # mv renames when it can. Across filesystems rename(2) answers EXDEV, and
  # there the move is a copy followed by a delete -- without the fallback,
  # moving out of /tmp onto another mount raised where CRuby succeeds.
  def mv(src, dest, force: nil, verbose: nil, noop: nil, secure: nil)
    fu_output_message("mv#{force ? ' -f' : ''} #{(fu_list(src) + [dest.to_s]).join(' ')}") if verbose
    return nil if noop
    fu_list(src).each do |s|
      d = fu_dest_path(s, dest.to_s)
      begin
        File.rename(s, d)
      rescue StandardError
        fu_copy_tree(s, d)
        fu_rm_tree(s, true)
      end
    end
    nil
  end

  def move(src, dest, force: nil, verbose: nil, noop: nil, secure: nil)
    mv(src, dest, force: force, verbose: verbose, noop: noop)
  end

  # install copies each source unless the destination already holds the same
  # contents, making the destination's directory first, then gives the copy
  # mode (read against the source, for a symbolic one) and, with preserve, the source's
  # times. A list of sources, or a destination that is a directory, takes
  # each source's basename inside it. There is no owner: or group:, which
  # would be chown.
  def install(src, dest, mode: nil, preserve: nil, noop: nil, verbose: nil)
    if verbose
      flags = "#{preserve ? ' -p' : ''}#{mode ? " -m #{fu_mode_to_s(mode)}" : ''}"
      fu_output_message("install -c#{flags} #{(fu_list(src) + [dest.to_s]).join(' ')}")
    end
    return nil if noop
    into = src.is_a?(Array) || File.directory?(dest.to_s)
    fu_list(src).each do |s|
      d = into ? File.join(dest.to_s, File.basename(s)) : dest.to_s
      raise ArgumentError, "same file: #{s} and #{d}" if File.identical?(s, d)
      st = File.stat(s)
      next if File.exist?(d) && compare_file(s, d)
      remove_file(d, true)
      # a destination spelled with a trailing slash is a directory to make
      if d.end_with?("/")
        mkdir_p(d)
        copy_file(s, d + File.basename(s))
      else
        mkdir_p(File.expand_path("..", d))
        copy_file(s, d)
      end
      File.utime(st.atime, st.mtime, d) if preserve
      File.chmod(fu_mode(mode, s), d) if mode
    end
    nil
  end

  # touch creates what is missing and updates the times of what is not.
  def touch(list, mtime: nil, nocreate: nil, verbose: nil, noop: nil)
    t = mtime || Time.now
    if verbose
      fu_output_message("touch #{nocreate ? '-c ' : ''}#{mtime ? mtime.strftime('-t %Y%m%d%H%M.%S ') : ''}#{fu_list(list).join(' ')}")
    end
    return fu_list(list) if noop
    fu_list(list).each do |path|
      if File.exist?(path)
        File.utime(t, t, path)
      else
        next if nocreate
        File.open(path, "w") { |f| }
      end
    end
    fu_list(list)
  end

  def ln_s(src, dest, force: nil, verbose: nil, noop: nil)
    fu_output_message("ln -s#{force ? 'f' : ''} #{(fu_list(src) + [dest.to_s]).join(' ')}") if verbose
    return nil if noop
    fu_list(src).each do |s|
      d = fu_dest_path(s, dest.to_s)
      begin
        File.delete(d)
      rescue StandardError
        # nothing there to unlink, which is the ordinary case
      end if force
      File.symlink(s, d)
    end
    nil
  end

  def symlink(src, dest, force: nil, verbose: nil, noop: nil)
    ln_s(src, dest, force: force, verbose: verbose, noop: noop)
  end

  def ln(src, dest, force: nil, verbose: nil, noop: nil)
    fu_output_message("ln#{force ? ' -f' : ''} #{(fu_list(src) + [dest.to_s]).join(' ')}") if verbose
    return nil if noop
    fu_list(src).each { |s| File.link(s, fu_dest_path(s, dest.to_s)) }
    nil
  end

  def link(src, dest, force: nil, verbose: nil, noop: nil)
    ln(src, dest, force: force, verbose: verbose, noop: noop)
  end

  def chmod(mode, list, verbose: nil, noop: nil)
    fu_output_message("chmod #{fu_mode_to_s(mode)} #{fu_list(list).join(' ')}") if verbose
    return fu_list(list) if noop
    fu_list(list).each { |path| File.chmod(fu_mode(mode, path), path) }
    fu_list(list)
  end

  def chmod_R(mode, list, verbose: nil, noop: nil, force: nil)
    fu_output_message("chmod -R#{force ? 'f' : ''} #{fu_mode_to_s(mode)} #{fu_list(list).join(' ')}") if verbose
    return fu_list(list) if noop
    fu_list(list).each { |path| fu_chmod_tree(mode, path) }
    fu_list(list)
  end

  def fu_chmod_tree(mode, path)
    File.chmod(fu_mode(mode, path), path)
    return nil unless File.directory?(path) && !File.symlink?(path)
    Dir.entries(path).each do |e|
      next if e == "." || e == ".."
      fu_chmod_tree(mode, "#{path}/#{e}")
    end
    nil
  end

  def cd(dir, verbose: nil, &block)
    fu_output_message("cd #{dir}") if verbose
    result = Dir.chdir(dir.to_s, &block)
    fu_output_message("cd -") if verbose && block
    result
  end

  def chdir(dir, verbose: nil, &block)
    cd(dir, verbose: verbose, &block)
  end

  def pwd
    Dir.pwd
  end

  def getwd
    Dir.pwd
  end

  def identical?(a, b)
    return false unless File.exist?(a.to_s) && File.exist?(b.to_s)
    File.read(a.to_s) == File.read(b.to_s)
  end

  def compare_file(a, b)
    identical?(a, b)
  end

  def uptodate?(new, old_list)
    return false unless File.exist?(new.to_s)
    t = File.mtime(new.to_s)
    fu_list(old_list).each do |o|
      next unless File.exist?(o)
      return false if File.mtime(o) > t
    end
    true
  end
end
