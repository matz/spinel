# Spinel bundled `tempfile` -- a file with a unique name, opened for
# reading and writing and removed when the block ends.
#
# `create` IS THE WHOLE SURFACE, and that is a property of the runtime
# rather than a choice. CRuby's `Tempfile.new` hands back an object
# whose file is removed by a finalizer when the object is collected;
# spinel has no `ObjectSpace.define_finalizer`, so a `new` here would
# return a file that nothing ever deletes -- one leaked file per call,
# and quieter than the NameError it replaced. `create`'s block IS that
# lifetime written down, and it is the form CRuby's own documentation
# recommends.
#
# THE EXCLUSIVE CREATE IS THE POINT, not the unique name. The name is
# guessable -- it carries the date, the pid and 32 bits of `rand`, the
# same shape CRuby's `Dir::Tmpname` builds -- so what keeps a caller
# from being handed a file someone else prepared is O_EXCL, which the
# "x" mode character asks for: a name that already exists is an
# Errno::EEXIST and another attempt, never a handle onto their file.
#
# NOT ACCEPTED, and each for a reason in the runtime rather than in the
# corpus:
#
# * `mode:`, CRuby's extra integer open flags. `File::RDWR` and its
#   siblings are not defined in spinel, so a caller has nothing to pass
#   and nothing to combine. `binmode:` is accepted instead, which is
#   the one option of the set that means anything on a POSIX open.
# * An `encoding:`/`textmode:` pair, for the same reason.
#
# THE ONE DIVERGENCE WORTH NAMING. CRuby's block form asks
# `File.identical?(file, file.path)` before it unlinks, so a block that
# renamed something else onto its own temp path does not have that
# other file removed underneath it. `File.identical?` here takes two
# paths and not an open file, so the question cannot be put; this
# closes and then unlinks by name. Reaching the difference takes a
# block that replaced its own temp file, which is not an accident a
# caller has.
require "tmpdir"

class Tempfile
  # CRuby's Dir::Tmpname retry bound, and the same one packages/tmpdir
  # uses for directories.
  MAX_TRY = 10000

  def self.create(basename = "", tmpdir = nil, max_try: nil, binmode: false, perm: 0600, &block)
    prefix, suffix = split_basename(basename)
    parent = tmpdir ? File.path(tmpdir) : Dir.tmpdir
    raise ArgumentError, "empty parent path" if parent.empty?

    tries = max_try || MAX_TRY
    raise ArgumentError, "max_try must be positive" if tries < 1

    # "w" so the open creates, "+" so the caller can read back what it
    # wrote -- a temp file written and then handed to something that
    # reads it is the ordinary use -- and "x" for O_EXCL. The mode
    # truncates, which on a file that cannot already exist is nothing.
    mode = binmode ? "wbx+" : "wx+"
    date = Time.now.strftime("%Y%m%d")

    file = nil
    i = 0
    while i < tries
      # CRuby appends the counter only from the second attempt, so the
      # common case is the name without one.
      counter = i == 0 ? "" : "-#{i}"
      path = "#{parent}/#{prefix}#{date}-#{Process.pid}-" \
             "#{random_token}#{counter}#{suffix}"
      begin
        file = File.open(path, mode, perm)
        break
      rescue Errno::EEXIST
        i += 1
      end
    end
    raise Errno::EEXIST, "cannot generate temporary file name" if file.nil?

    return file unless block

    path = file.path
    begin
      yield file
    ensure
      # CLOSE AND UNLINK, BOTH IN THE ENSURE. A block that raised still
      # holds an open descriptor and still has a file on disk, and a
      # test suite that leaks one per assertion runs a machine out of
      # both. A caller that closed the file itself is the ordinary case
      # and not an error.
      file.close unless file.closed?
      begin
        File.unlink(path)
      rescue Errno::ENOENT
        # The block removed or renamed it. Nothing left to do.
      end
    end
  end

  # 32 bits of `rand` in base 36, drawn as two halves. CRuby writes this
  # as `rand(0x100000000)`, and that literal does not fit an Integer on
  # the 32-bit build -- two draws below 2**16 carry the same entropy and
  # are representable at every width the compiler targets. The rendering
  # is up to eight base-36 characters where CRuby's is up to seven;
  # nothing reads the token, and what keeps two callers apart is the
  # exclusive create rather than the width of this number.
  def self.random_token
    rand(0x10000).to_s(36) + rand(0x10000).to_s(36)
  end

  # `"probe"` or `%w[probe .img]` -- CRuby accepts either, and the array
  # form is how a caller asks for a SUFFIX, which matters when whatever
  # reads the file back looks at its extension.
  def self.split_basename(basename)
    if basename.is_a?(Array)
      prefix = basename.length > 0 ? basename[0].to_s : ""
      suffix = basename.length > 1 ? basename[1].to_s : ""
      return [prefix, suffix]
    end
    [basename.to_s, ""]
  end
end
