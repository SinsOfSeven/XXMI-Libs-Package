#include "ShaderRegex.h"
#include "ShaderStage.h"
#include "BlobStore.h"
#include "ShaderBytecodeValidation.h"
#include "CommandList.h"
#include "Overlay.h"
#include "globals.h" // For ShaderOverride FIXME: This should be in a separate header
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

ShaderRegexGroups shader_regex_groups;
std::vector<ShaderRegexGroup*> shader_regex_group_index;
uint32_t shader_regex_hash;

namespace {
	struct CompiledRegexCacheEntry {
		std::shared_ptr<pcre2_code> code;
		uint64_t last_used_reload = 0;
	};
	std::map<std::string, CompiledRegexCacheEntry> compiled_regex_cache;
	uint64_t regex_cache_reload = 0;
	const uint32_t regex_compile_options = PCRE2_CASELESS | PCRE2_MULTILINE;
}

void begin_shader_regex_compile_cache_reload()
{
	++regex_cache_reload;
}

void finish_shader_regex_compile_cache_reload()
{
	for (auto entry = compiled_regex_cache.begin(); entry != compiled_regex_cache.end();) {
		if (entry->second.last_used_reload != regex_cache_reload)
			entry = compiled_regex_cache.erase(entry);
		else
			++entry;
	}
}

static void log_pcre2_error_nonl(int err, char *fmt, ...)
{
	PCRE2_UCHAR buf[120]; // doco says "120 code units is ample"
	va_list ap;

	pcre2_get_error_message(err, buf, sizeof(buf));

	va_start(ap, fmt);
	vLogWarning(fmt, ap);
	va_end(ap);

	LogWarning(": %s\n", buf);
}

static bool get_shader_model(std::string *asm_text, std::string *shader_model)
{
	size_t shader_model_pos;

	for (
		shader_model_pos = asm_text->find("\n");
		shader_model_pos != std::string::npos && (*asm_text)[shader_model_pos + 1] == '/';
		shader_model_pos = asm_text->find("\n", shader_model_pos + 1)
	) {}

	if (shader_model_pos == std::string::npos)
		return false;

	*shader_model = asm_text->substr(shader_model_pos + 1, asm_text->find("\n", shader_model_pos + 1) - shader_model_pos - 1);
	return true;
}

static bool find_dcl_end(std::string *asm_text, size_t *dcl_end_pos)
{
	// FIXME: Might be better to scan forwards

	*dcl_end_pos = asm_text->rfind("\ndcl_");
	*dcl_end_pos = asm_text->find("\n", *dcl_end_pos + 1);

	if (*dcl_end_pos == std::string::npos) {
		LogWarning("Unable to locate end of shader declarations while applying ShaderRegex edits\n");
		return false;
	}

	return true;
}

static bool insert_declarations(std::string *asm_text, ShaderRegexDeclarations *declarations)
{
	ShaderRegexDeclarations::iterator i;
	std::string insert_str;
	size_t dcl_end;
	bool patch = false;

	if (!find_dcl_end(asm_text, &dcl_end))
		return false;

	for (i = declarations->begin(); i != declarations->end(); i++) {
		insert_str = std::string("\n") + *i;

		if (asm_text->find(insert_str + std::string("\n")) != std::string::npos)
			continue;

		asm_text->insert(dcl_end, insert_str);
		dcl_end += insert_str.size();

		patch = true;
	}

	return patch;
}

static bool find_dcl_temps(std::string *asm_text, size_t *dcl_temps_pos)
{
	// Could use regex for this as well, but given we only need to find a
	// constant string it will be more efficient to just do this:
	*dcl_temps_pos = asm_text->find("\ndcl_temps ", 0);

	if (*dcl_temps_pos == std::string::npos)
		return false;

	return true;
}

static unsigned get_dcl_temps(std::string *asm_text)
{
	size_t dcl_temps;
	unsigned tmp_regs = 0;

	if (!find_dcl_temps(asm_text, &dcl_temps))
		return 0;

	tmp_regs = stoul(asm_text->substr(dcl_temps + 10, 4));
	LogDebug("Found dcl_temps %d\n", tmp_regs);

	return tmp_regs;
}

static bool update_dcl_temps(std::string *asm_text, size_t new_val)
{
	size_t dcl_temps, dcl_temps_end, dcl_end;
	std::string insert_str;

	if (find_dcl_temps(asm_text, &dcl_temps)) {
		dcl_temps += 11;
		dcl_temps_end = asm_text->find("\n", dcl_temps);
		LogDebug("Updating dcl_temps %Iu\n", new_val);
		asm_text->replace(dcl_temps, dcl_temps_end - dcl_temps, std::to_string(new_val));
		return true;
	}

	if (!find_dcl_end(asm_text, &dcl_end))
		return false;

	insert_str = std::string("\ndcl_temps ") + std::to_string(new_val);
	LogDebug("Inserting dcl_temps %Iu\n", new_val);
	asm_text->insert(dcl_end, insert_str);
	dcl_end += insert_str.size();

	return true;
}

ShaderRegexPattern::ShaderRegexPattern() :
	do_replace(false)
{
}

bool ShaderRegexPattern::compile(std::string *pattern)
{
	uint32_t name_table_entry_size;
	uint32_t name_table_count;
	uint32_t i;
	PCRE2_SPTR name_table;
	PCRE2_SIZE err_off;
	int err;

	// CASELESS is for compatibility with d3dcompiler_46 & 47 without
	// having to always remember to account for the dcl_constantbuffer
	// differences:
	auto cached = compiled_regex_cache.find(*pattern);
	if (cached != compiled_regex_cache.end()) {
		regex = cached->second.code;
		cached->second.last_used_reload = regex_cache_reload;
	} else {
		pcre2_code *compiled = pcre2_compile((PCRE2_SPTR)pattern->c_str(),
				pattern->length(), // or PCRE2_ZERO_TERMINATED
				regex_compile_options,
				&err, &err_off, NULL);
		if (!compiled) {
			log_pcre2_error_nonl(err, "  WARNING: PCRE2 regex compilation failed at offset %u", (unsigned)err_off);
			return false;
		}
		regex = std::shared_ptr<pcre2_code>(compiled, pcre2_code_free);

		// COMPLETE enables JIT for the matching mode used by pcre2_match.
		// JIT failure is non-fatal: pcre2_match can use the interpreter.
		int jit_err = pcre2_jit_compile(regex.get(), PCRE2_JIT_COMPLETE);
		if (jit_err < 0)
			LogDebug("PCRE2 JIT compilation unavailable (%d); using interpreter when needed\n", jit_err);

		compiled_regex_cache[*pattern] = { regex, regex_cache_reload };
	}

	pcre2_pattern_info(regex.get(), PCRE2_INFO_NAMECOUNT, &name_table_count);
	pcre2_pattern_info(regex.get(), PCRE2_INFO_NAMEENTRYSIZE, &name_table_entry_size);
	pcre2_pattern_info(regex.get(), PCRE2_INFO_NAMETABLE, &name_table);

	static_assert(PCRE2_CODE_UNIT_WIDTH == 8, "Need to fix name table parsing for non-8bit pcre2");
	for (i = 0; i < name_table_count; i++)
		named_capture_groups.insert(std::string((char*)(name_table + name_table_entry_size*i + 2)));

	return true;
}

bool ShaderRegexPattern::named_group_overlaps(ShaderRegexTemps &other_set)
{
	ShaderRegexTemps intersection;

	// C++ why you be so verbose?
	std::set_intersection(
				named_capture_groups.begin(),
				named_capture_groups.end(),
				other_set.begin(),
				other_set.end(),
				std::inserter(intersection, intersection.begin()));

	return intersection.size() != 0;
}

bool ShaderRegexPattern::matches(std::string *asm_text)
{
	pcre2_match_data *match_data = NULL;
	bool match = false;
	int rc;

	// TODO: Assign per-thread JIT stack if the default 32K turns out to be
	// insufficient. Can probably store this in the context, as that is
	// supposed to be per-thread, or use thread local storage.

	match_data = pcre2_match_data_create_from_pattern(regex.get(), NULL);

	// pcre2_match uses the compiled JIT code when this matching mode is
	// supported and otherwise falls back to the interpreter.
	rc = pcre2_match(regex.get(), (PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0, 0, match_data, NULL);
	if (rc == PCRE2_ERROR_NOMATCH)
		goto out_free;
	if (rc < 0) {
		log_pcre2_error_nonl(rc, "  WARNING: regex match error");
		goto out_free;
	}

	match = true;

out_free:
	pcre2_match_data_free(match_data);
	return match;
}

static void replacement_search_and_replace(std::string &str, std::string *search, std::string *replace)
{
	size_t pos;

	for (pos = str.find(*search); pos != std::string::npos; pos = str.find(*search, pos + 1)) {
		if (pos > 0 && (str[pos-1] == '$' || str[pos-1] == '\\'))
			continue;

		str.replace(pos, search->length(), *replace);
	}
}

static void substitute_temp_regs(std::string &replacement, ShaderRegexTemps *temp_regs, unsigned dcl_temps)
{
	ShaderRegexTemps::iterator i;
	unsigned tmp_reg = dcl_temps;
	std::string search_str, repl_str;

	for (i = temp_regs->begin(); i != temp_regs->end(); i++, tmp_reg++) {
		repl_str = std::string("r") + std::to_string(tmp_reg);

		search_str = std::string("$") + *i;
		replacement_search_and_replace(replacement, &search_str, &repl_str);

		search_str = std::string("${") + *i + std::string("}");
		replacement_search_and_replace(replacement, &search_str, &repl_str);
	}
}

bool ShaderRegexPattern::patch(std::string *asm_text, ShaderRegexTemps *temp_regs, unsigned dcl_temps)
{
	pcre2_match_data *match_data = NULL;
	PCRE2_SIZE est_size, output_size;
	std::string replace_copy;
	PCRE2_UCHAR *buf = NULL;
	bool patch = false;
	uint32_t options;
	int rc;

	static_assert(PCRE2_CODE_UNIT_WIDTH == 8, "Need to fix output buffer allocation for non-8bit pcre2");

	// We operate on a copy of the replace string so that future shaders
	// don't get our temporary register numbers:
	replace_copy = replace;
	substitute_temp_regs(replace_copy, temp_regs, dcl_temps);

	// TODO: Allow named capture groups from other patterns in the same
	// regex group to be substituted in, and provide some simple arithmetic
	// operators to e.g. allow a constant buffer byte offset to be divided
	// by 16 to get the constant buffer index and vice versa

	// At a minimum we want \n to be translated in the replace string,
	// which needs extended substitution processing to be enabled:
	options = PCRE2_SUBSTITUTE_EXTENDED;

	match_data = pcre2_match_data_create_from_pattern(regex.get(), NULL);

	output_size = est_size = asm_text->length() + replace_copy.length() + 1024;
	buf = new PCRE2_UCHAR[output_size];
	rc = pcre2_substitute(regex.get(),
			(PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0,
			options | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
			match_data, NULL,
			(PCRE2_SPTR)replace_copy.c_str(), replace_copy.length(),
			buf, &output_size);

	if (rc == PCRE2_ERROR_NOMEMORY) {
		LogWarning("  NOTICE: regex replace requires a %u byte buffer\n", (unsigned)output_size);
		LogWarning("  NOTICE: We underestimated by %u bytes and have to start over\n", (unsigned)(output_size - est_size));
		LogWarning("  NOTICE: What kind of crazy are you doing to get down this code path?\n");
		LogWarning("  NOTICE: You didn't inject a matrix inverse or two in assembly did you?\n");
		LogWarning("  NOTICE: Once more, with passion!\n");

		delete [] buf;
		buf = new PCRE2_UCHAR[output_size];

		rc = pcre2_substitute(regex.get(),
				(PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0,
				options, // No PCRE2_SUBSTITUTE_OVERFLOW_LENGTH this time
				match_data, NULL,
				(PCRE2_SPTR)replace_copy.c_str(), replace_copy.length(),
				buf, &output_size);
	}

	if (rc == 0)
		goto out_free;
	if (rc < 0) {
		log_pcre2_error_nonl(rc, "  WARNING: regex replace error");
		goto out_free;
	}

	*asm_text = (char*)buf;
	patch = true;

out_free:
	pcre2_match_data_free(match_data);
	delete [] buf;

	return patch;
}

void ShaderRegexGroup::apply_regex_patterns(std::string *asm_text, bool *match, bool *patch)
{
	ShaderRegexPatterns::iterator i;
	ShaderRegexPattern *pattern;
	unsigned dcl_temps = 0;

	// Match defaults to true so that if there are no patterns we can still
	// apply the command list. Patch defaults to false because we don't
	// want to waste time re-assembling the shader if we didn't change it.
	*match = true;
	*patch = false;

	if (!temp_regs.empty())
		dcl_temps = get_dcl_temps(asm_text);

	for (i = patterns.begin(); i != patterns.end(); i++) {
		pattern = &i->second;

		if (pattern->do_replace)
			*match = *patch = pattern->patch(asm_text, &temp_regs, dcl_temps);
		else
			*match = pattern->matches(asm_text);

		if (!*match) {
			*patch = false;
			return;
		}
	}

	// Only update dcl_temps if we are patching:
	if (*patch && !temp_regs.empty())
		*patch = update_dcl_temps(asm_text, dcl_temps + temp_regs.size());

	// But we can update declarations even if we aren't doing a regex
	// replace in some cases, so long as the patterns all matched (e.g.
	// globally disable the driver stereo cb):
	if (!declarations.empty())
		*patch = insert_declarations(asm_text, &declarations) || *patch;
}

void ShaderRegexGroup::link_command_lists_and_filter_index(UINT64 shader_hash)
{
	ShaderOverride *shader_override = NULL;
	wstring ini_section, ini_line;
	CommandList::Commands::reverse_iterator i;

	// Only link the command lists if we have something in ours to link in,
	// because this will create ShaderOverride sections for shaders that
	// don't already have one, adding more work in the draw calls.

	if (command_list.noop() && post_command_list.noop() && filter_index == FLT_MAX)
		return;

	shader_override = &G->mShaderOverrideMap[shader_hash];

	// Initialise the ShaderOverride's command lists if they aren't already:
	if (shader_override->command_list.ini_section.empty()) {
		ini_section = command_list.ini_section + L".Match";
		shader_override->command_list.ini_section = ini_section;
		shader_override->post_command_list.ini_section = ini_section;
		shader_override->post_command_list.post = true;
	}

	// Set the filter index for partner filtering:
	if (shader_override->filter_index == FLT_MAX)
		shader_override->filter_index = filter_index;

	// If we have previously linked a command list (on any matched shader)
	// we will reuse the link command here, after checking that this
	// matched shader has not already been linked. Avoids the command lists
	// growing endlessly and eventually killing performance.
	if (link) {
		for (i = shader_override->command_list.commands.rbegin();
		         i != shader_override->command_list.commands.rend(); i++) {
			if (*i == link)
				return;
		}
		shader_override->command_list.commands.push_back(link);
		if (post_link)
			shader_override->post_command_list.commands.push_back(post_link);
		return;
	} else if (post_link) {
		for (i = shader_override->post_command_list.commands.rbegin();
		         i != shader_override->post_command_list.commands.rend(); i++) {
			if (*i == post_link)
				return;
		}
		shader_override->post_command_list.commands.push_back(post_link);
		return;
	}

	// This is the first shader this pattern has matched. Create a new
	// RunLinkedCommandList command and link it up:
	ini_line = L"[" + command_list.ini_section + L".Match] run = linked command list";

	if (!command_list.noop())
		link = LinkCommandLists(&shader_override->command_list, &command_list, &ini_line);

	if (!post_command_list.noop())
		post_link = LinkCommandLists(&shader_override->post_command_list, &post_command_list, &ini_line);
}

bool unlink_shader_regex_command_lists_and_filter_index(UINT64 shader_hash)
{
	ShaderOverride *shader_override = NULL;
	CommandList::Commands::iterator i, next;
	RunLinkedCommandList *link;
	bool ret = false;

	auto shader_override_i = G->mShaderOverrideMap.find(shader_hash);
	if (shader_override_i == G->mShaderOverrideMap.end())
		return false;

	shader_override = &shader_override_i->second;

	for (i = shader_override->command_list.commands.begin(), next = i;
	    i != shader_override->command_list.commands.end(); i = next) {
		next++;
		link = dynamic_cast<RunLinkedCommandList*>(i->get());
		if (link) {
			next = shader_override->command_list.commands.erase(i);
			ret = true;
		}
	}

	for (i = shader_override->post_command_list.commands.begin(), next = i;
	    i != shader_override->post_command_list.commands.end(); i = next) {
		next++;
		link = dynamic_cast<RunLinkedCommandList*>(i->get());
		if (link) {
			next = shader_override->post_command_list.commands.erase(i);
			ret = true;
		}
	}

	if (shader_override->filter_index != shader_override->backup_filter_index) {
		shader_override->filter_index = shader_override->backup_filter_index;
		ret = true;
	}

	return ret;
}

#define SHADER_REGEX_CACHE_VERSION 2
#define SHADER_REGEX_CACHE_MAGIC ('S' | ('R' << 8) | ('C' << 16) | ('X' << 24))

// The packed ShaderRegex cache (version 2). Replaces the old per-shader
// "<hash>-<type>_regex.dat/.bin" files. All the metadata for every shader
// lives in a single "ShaderRegexCache.dat" file (a cacheline friendly header,
// a flat array of fixed size records sorted for binary search and a flat pool
// of match ids), which is loaded into memory in one go. The patched bytecode
// lives in a single grow-only "ShaderRegexCache.blob" file made up of 4KiB
// aligned blocks that are streamed through a small sliding window on demand,
// so the whole blob is never loaded into memory and repeated accesses to the
// same block (e.g. after a config reload re-links the command lists) are
// served straight from the window. The blob is grow-only: dead blocks (a
// re-saved record or an unpatched record) are simply skipped over by the
// record offsets, and the whole pair is compacted by wiping whenever the
// ShaderRegex generation changes.
struct ShaderRegexCacheHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t shader_regex_hash;
	uint32_t record_count;
	uint64_t blob_size;
	uint32_t match_pool_count;
	uint32_t flags;
};

struct ShaderRegexCacheRecord {
	uint64_t hash;
	uint32_t match_pool_index;
	uint32_t match_count;
	uint64_t blob_offset;
	uint32_t blob_size;
	ShaderStage shader_type; // single byte, on-disk layout unchanged
	uint8_t patched;
	uint16_t _pad;
};

static_assert(sizeof(ShaderRegexCacheRecord) == 32,
		"ShaderRegexCacheRecord must be 32 bytes");

static bool shader_regex_cache_record_less(const ShaderRegexCacheRecord &a, const ShaderRegexCacheRecord &b)
{
	if (a.shader_type != b.shader_type)
		return a.shader_type < b.shader_type;
	return a.hash < b.hash;
}

static void shader_regex_cache_get_paths(wchar_t *dat_path, wchar_t *blob_path)
{
	// Zero first so a failed swprintf_s can't leave a garbage path behind:
	dat_path[0] = 0;
	if (blob_path)
		blob_path[0] = 0;

	swprintf_s(dat_path, MAX_PATH, L"%ls\\ShaderRegexCache.dat", G->SHADER_CACHE_PATH);
	if (blob_path)
		swprintf_s(blob_path, MAX_PATH, L"%ls\\ShaderRegexCache.blob", G->SHADER_CACHE_PATH);
}

// Holds the packed ShaderRegex cache: the in-memory record index (header +
// sorted fixed-size records + flat match-id pool) and the grow-only bytecode
// blob streamed through a small sliding window. All state here is guarded by
// shader_regex_cache_mutex; the public load/save_meta/save_bin/flush methods
// assume the lock is already held by their free-function wrappers.
//
// Note that save_meta() and save_bin() are two separate lock acquisitions with
// the assembly of the patched bytecode running between them, so a per-frame
// flush landing in that window can persist a PATCH record whose blob_size is
// still 0. That is a recoverable state, not a corrupt one: load() treats a
// zero blob_size on a patched record as a miss and the shader is re-analysed.
class ShaderRegexCacheStore {
public:
	ShaderRegexCache load(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode,
			std::wstring *tagline, ShaderRegexCacheFailureReason *reason);
	void save_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
			bool patched, std::string *asm_text, std::wstring *tagline);
	void save_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode);

	// Writes the metadata out if new records were queued since the last
	// flush. Called from the per-frame seam so the on-disk index stays at
	// most one frame behind the in-memory state (the blob itself is always
	// appended to immediately):
	bool flush();

	// Safe to call without the mutex. The per-frame flush uses this to skip
	// the lock entirely when there is nothing to write, so the render thread
	// is not blocked behind an in-progress ensure_valid() file I/O:
	bool is_dirty() const { return metadata_dirty.load(std::memory_order_relaxed); }

private:
	bool loaded = false;
	std::wstring metadata_path;
	ShaderRegexCacheHeader header;
	std::vector<ShaderRegexCacheRecord> records;
	std::vector<uint32_t> match_pool;
	// Atomic so is_dirty() can be read without the mutex; every mutation still
	// happens under shader_regex_cache_mutex.
	std::atomic<bool> metadata_dirty{ false };

	// Grow-only bytecode blob, streamed through a small sliding window (see
	// BlobStore.h). Shared between the cache stores so the same machinery
	// backs the ShaderBytecodeRecord ledger as well:
	BlobStore blob;

	std::vector<ShaderRegexCacheRecord>::iterator lower_bound(ShaderStage shader_type, uint64_t hash);
	ShaderRegexCacheRecord *find_record(ShaderStage shader_type, uint64_t hash);
	ShaderRegexCacheRecord *find_or_insert_record(ShaderStage shader_type, uint64_t hash);
	bool persist_metadata();
	bool metadata_load(const wchar_t *dat_path);
	bool validate_blob();
	void reset_state();
	void wipe_files();
	bool create_metadata();
	bool ensure_valid(bool create_if_missing, ShaderRegexCacheFailureReason *reason = NULL);
};

// Protects the store from the per-frame flush, which runs outside the global
// critical section. Linking command lists separately requires mCriticalSection.
// Lock order: mCriticalSection -> shader_regex_cache_mutex, never the reverse.
static std::mutex shader_regex_cache_mutex;
static ShaderRegexCacheStore shader_regex_cache_store;

// load() has a single caller and one optional out-param, so funnel every
// failure through here rather than repeating the null check at each site:
static ShaderRegexCache shader_regex_cache_fail(ShaderRegexCacheFailureReason *reason, ShaderRegexCacheFailureReason why)
{
	if (reason)
		*reason = why;
	return ShaderRegexCache::NO_CACHE;
}

std::vector<ShaderRegexCacheRecord>::iterator ShaderRegexCacheStore::lower_bound(ShaderStage shader_type, uint64_t hash)
{
	ShaderRegexCacheRecord key;
	memset(&key, 0, sizeof(key));
	key.shader_type = shader_type;
	key.hash = hash;

	return std::lower_bound(records.begin(), records.end(),
			key, shader_regex_cache_record_less);
}

ShaderRegexCacheRecord *ShaderRegexCacheStore::find_record(ShaderStage shader_type, uint64_t hash)
{
	std::vector<ShaderRegexCacheRecord>::iterator i = lower_bound(shader_type, hash);

	if (i == records.end() || i->shader_type != shader_type || i->hash != hash)
		return NULL;
	return &*i;
}

ShaderRegexCacheRecord *ShaderRegexCacheStore::find_or_insert_record(ShaderStage shader_type, uint64_t hash)
{
	std::vector<ShaderRegexCacheRecord>::iterator i = lower_bound(shader_type, hash);

	if (i != records.end() && i->shader_type == shader_type && i->hash == hash)
		return &*i;

	ShaderRegexCacheRecord record;
	memset(&record, 0, sizeof(record));
	record.hash = hash;
	record.shader_type = shader_type;

	return &*records.insert(i, record);
}

bool ShaderRegexCacheStore::flush()
{
	if (!metadata_dirty)
		return true;
	return persist_metadata();
}

bool ShaderRegexCacheStore::persist_metadata()
{
	const wchar_t *dat_path = metadata_path.c_str();
	size_t size;
	std::vector<byte> out;

	header.record_count = (uint32_t)records.size();
	header.match_pool_count = (uint32_t)match_pool.size();

	// The metadata is persisted by blob_store_metadata_write() to a temp
	// file that is atomically renamed over the real file so a crash at any
	// point can never leave a half-written cache behind. The blob is always
	// appended to *before* this runs, so the persisted metadata never
	// references bytecode that isn't on disk yet:
	size = sizeof(ShaderRegexCacheHeader)
			+ records.size() * sizeof(ShaderRegexCacheRecord)
			+ match_pool.size() * sizeof(uint32_t);
	if (size > BLOB_STORE_METADATA_MAX)
		return false;

	out.resize(size);

	const size_t records_offset = sizeof(ShaderRegexCacheHeader);
	const size_t pool_offset = records_offset + records.size() * sizeof(ShaderRegexCacheRecord);

	memcpy(out.data(), &header, sizeof(ShaderRegexCacheHeader));
	if (!records.empty())
		memcpy(out.data() + records_offset, records.data(), records.size() * sizeof(ShaderRegexCacheRecord));
	if (!match_pool.empty())
		memcpy(out.data() + pool_offset, match_pool.data(), match_pool.size() * sizeof(uint32_t));

	if (!blob_store_metadata_write(dat_path, out.data(), out.size())) {
		LogWarning("ShaderRegexCache: persist_metadata write to %S FAILED (records=%u match_pool=%u size=%Iu)\n",
				dat_path, header.record_count, header.match_pool_count, size);
		return false;
	}
	metadata_dirty = false;
	return true;
}

// Trust boundary for a file that may be truncated, corrupt or tampered with.
// Everything is parsed and validated into locals first and only adopted once
// every record passes, so a false return leaves this store exactly as it was -
// the caller does not need a reset_state() to make a failed load safe.
bool ShaderRegexCacheStore::metadata_load(const wchar_t *dat_path)
{
	std::vector<byte> buf;

	if (!blob_store_metadata_read(dat_path, &buf))
		return false;

	if (buf.size() < sizeof(ShaderRegexCacheHeader))
		return false;

	ShaderRegexCacheHeader file_header;
	memcpy(&file_header, buf.data(), sizeof(file_header));

	if (file_header.magic != SHADER_REGEX_CACHE_MAGIC
	 || file_header.version != SHADER_REGEX_CACHE_VERSION)
		return false;

	// The file must be exactly its expected size (header + fixed size records +
	// flat match id pool) or it's not our file:
	size_t expected = sizeof(ShaderRegexCacheHeader)
			+ (size_t)file_header.record_count * sizeof(ShaderRegexCacheRecord)
			+ (size_t)file_header.match_pool_count * sizeof(uint32_t);
	if (buf.size() != expected)
		return false;

	// Everything is parsed and validated into locals first, and only adopted
	// once every record passes, so a rejected file cannot leave the store
	// holding a mix of old and new state:
	std::vector<ShaderRegexCacheRecord> loaded_records(file_header.record_count);
	std::vector<uint32_t> loaded_pool(file_header.match_pool_count);

	if (!loaded_records.empty()) {
		memcpy(loaded_records.data(),
				buf.data() + sizeof(ShaderRegexCacheHeader),
				loaded_records.size() * sizeof(ShaderRegexCacheRecord));
	}
	if (!loaded_pool.empty()) {
		memcpy(loaded_pool.data(),
				buf.data() + sizeof(ShaderRegexCacheHeader)
					+ loaded_records.size() * sizeof(ShaderRegexCacheRecord),
				loaded_pool.size() * sizeof(uint32_t));
	}

	// Validate every record: the (stage, hash) sort invariant the binary search
	// relies on, plus all the pool and blob ranges. Anything off means the file
	// was tampered with or truncated:
	ShaderRegexCacheRecord prev;
	for (size_t i = 0; i < loaded_records.size(); i++) {
		ShaderRegexCacheRecord &r = loaded_records[i];

		if (i && !shader_regex_cache_record_less(prev, r))
			return false;
		if (r.shader_type >= ShaderStage::COUNT)
			return false;
		if (!blob_store_range_valid(r.match_pool_index, r.match_count, file_header.match_pool_count))
			return false;
		if (r.blob_size
		 && (r.blob_offset % BlobStore::block_alignment()
		 || !blob_store_range_valid(r.blob_offset, r.blob_size, file_header.blob_size)))
			return false;
		if (r.patched > 1 || (r.patched && !r.match_count))
			return false;

		prev = r;
	}

	this->header = file_header;
	records.swap(loaded_records);
	match_pool.swap(loaded_pool);
	return true;
}

// Make sure the blob is consistent with the metadata before we trust it. The
// blob is grow-only, so it only ever needs to be at least as large as the
// metadata claims:
bool ShaderRegexCacheStore::validate_blob()
{
	return blob.validate(header.blob_size);
}

void ShaderRegexCacheStore::reset_state()
{
	blob.close();

	records.clear();
	match_pool.clear();
	loaded = false;
	metadata_dirty = false;
}

void ShaderRegexCacheStore::wipe_files()
{
	wchar_t dat_path[MAX_PATH], blob_path[MAX_PATH];

	shader_regex_cache_get_paths(dat_path, blob_path);

	reset_state();

	blob_store_delete(dat_path);
	blob_store_delete(blob_path);
}

bool ShaderRegexCacheStore::create_metadata()
{
	wchar_t dat_path[MAX_PATH];

	shader_regex_cache_get_paths(dat_path, NULL);

	memset(&header, 0, sizeof(ShaderRegexCacheHeader));
	header.magic = SHADER_REGEX_CACHE_MAGIC;
	header.version = SHADER_REGEX_CACHE_VERSION;
	header.shader_regex_hash = shader_regex_hash;
	loaded = true;
	metadata_dirty = true;

	if (!persist_metadata()) {
		LogWarning("ShaderRegexCache: create_metadata failed to persist %S (shader_regex_hash=%08x)\n",
				dat_path, shader_regex_hash);
		reset_state();
		return false;
	}

	LogInfo("ShaderRegexCache: cache created at %S (generation %08x)\n", dat_path, shader_regex_hash);
	return true;
}

// Loads the metadata into memory, or (re)creates it if create_if_missing.
// The cache is tied to the ShaderRegex generation hash: if it doesn't match
// the current shader_regex_hash the whole pair is discarded (and recreated
// from scratch on the next write) so no stale cache is ever used. On failure,
// reason (optional) distinguishes "what was on disk is not usable" from "it is
// usable but belongs to an older generation" - the two are worth telling apart,
// since the first can mean a truncated or damaged file.
bool ShaderRegexCacheStore::ensure_valid(bool create_if_missing, ShaderRegexCacheFailureReason *reason)
{
	wchar_t dat_path[MAX_PATH], blob_path[MAX_PATH];
	ShaderRegexCacheFailureReason local_reason = ShaderRegexCacheFailureReason::NONE;
	ShaderRegexCacheFailureReason *failure_reason = reason ? reason : &local_reason;

	shader_regex_cache_get_paths(dat_path, blob_path);
	const bool metadata_exists = GetFileAttributesW(dat_path) != INVALID_FILE_ATTRIBUTES;
	*failure_reason = ShaderRegexCacheFailureReason::NONE;
	if (blob.set_path(blob_path))
		reset_state();
	metadata_path = dat_path;

	// We may already have the cache loaded from a previous config reload. If
	// the ShaderRegex generation changed since then it's all stale:
	if (loaded) {
		if (header.shader_regex_hash == shader_regex_hash) {
			blob.set_logical_end(header.blob_size);
			return true;
		}

		if (!create_if_missing) {
			reset_state();
			*failure_reason = ShaderRegexCacheFailureReason::STALE_GENERATION;
			return false;
		}

		wipe_files();
	}

	if (!metadata_load(dat_path)) {
		reset_state();
		*failure_reason = ShaderRegexCacheFailureReason::CORRUPT_METADATA;
	} else if (header.shader_regex_hash != shader_regex_hash) {
		reset_state();
		*failure_reason = ShaderRegexCacheFailureReason::STALE_GENERATION;
	} else if (!validate_blob()) {
		reset_state();
		*failure_reason = ShaderRegexCacheFailureReason::CORRUPT_METADATA;
	} else {
		blob.set_logical_end(header.blob_size);
		loaded = true;
		LogInfo("ShaderRegexCache: loaded %u records from %S (blob %Iu, generation %08x)\n",
				(unsigned)records.size(), dat_path, (uint64_t)header.blob_size, header.shader_regex_hash);
		return true;
	}

	if (!create_if_missing)
		return false;

	if (metadata_exists && *failure_reason == ShaderRegexCacheFailureReason::CORRUPT_METADATA)
		LogWarning("ShaderRegexCache: invalid cache %S, recreating for generation %08x\n",
				dat_path, shader_regex_hash);
	else
		LogInfo("ShaderRegexCache: cache not present or stale at %S; creating generation %08x\n",
				dat_path, shader_regex_hash);
	wipe_files();

	return create_metadata();
}

// Returns the cache verdict for one shader. Note this is not a pure query: as a
// side effect it appends each matched group's ini section to tagline, and it
// re-links the matched groups' command lists into G->mShaderOverrideMap via
// link_command_lists_and_filter_index() so the shader draws as if its [ShaderOverride]
// had been parsed. The MATCH verdict at the call site is just "return early",
// so that work is not visible from there. reason (optional) receives why the
// verdict was NO_CACHE; the analysis path is identical either way, so it is a
// diagnostic only.
ShaderRegexCache ShaderRegexCacheStore::load(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode, std::wstring *tagline, ShaderRegexCacheFailureReason *reason)
{
	ShaderRegexGroup *group;
	ShaderRegexCacheRecord *record;
	ShaderStage stage;
	uint32_t i;

	if (reason)
		*reason = ShaderRegexCacheFailureReason::NONE;

	// Diagnostic option: never load from the packed cache, forcing the full
	// deferred analysis path (useful for testing without the cache):
	if (G->DISABLE_REGEX_CACHE)
		return shader_regex_cache_fail(reason, ShaderRegexCacheFailureReason::DISABLED);

	if (!G->SHADER_CACHE_PATH[0])
		return shader_regex_cache_fail(reason, ShaderRegexCacheFailureReason::NO_CACHE_PATH);

	stage = shader_stage_from_label(shader_type);
	if (stage == ShaderStage::INVALID)
		return shader_regex_cache_fail(reason, ShaderRegexCacheFailureReason::INVALID_STAGE);

	// ensure_valid() reports the specific reason the cache was not usable:
	if (!ensure_valid(false, reason))
		return ShaderRegexCache::NO_CACHE;

	record = find_record(stage, hash);
	if (!record)
		return shader_regex_cache_fail(reason, ShaderRegexCacheFailureReason::NO_RECORD);

	// num_matches may be 0, which means the ShaderRegex didn't match the
	// shader, but we cache it anyway to skip processing the shader again.
	// We don't really need any special handling for this case, since
	// returning MATCH will already skip that handling in the caller, but
	// we return a special value so the caller can log it appropriately.
	if (record->match_count == 0)
		return ShaderRegexCache::NO_MATCH;

	// Validate the entire hit before linking anything. A late failure must not
	// leave partial command-list links or a tagline for the fallback analysis.
	for (i = 0; i < record->match_count; i++) {
		if (match_pool[record->match_pool_index + i] >= shader_regex_group_index.size())
			return shader_regex_cache_fail(reason, ShaderRegexCacheFailureReason::MATCH_POOL_OOB);
	}
	if (record->patched) {
		if (!record->blob_size)
			return shader_regex_cache_fail(reason, ShaderRegexCacheFailureReason::RECORD_NO_BYTECODE);
		bytecode->resize(record->blob_size);
		uint32_t version;
		if (!blob.read(record->blob_offset, record->blob_size, bytecode->data())
		 || !shader_bytecode_dxbc_version(bytecode->data(), bytecode->size(), &version)
		 || shader_stage_from_dxbc_type(version >> 16) != stage
		 || !shader_bytecode_dxbc_instructions(bytecode->data(), bytecode->size()))
			return shader_regex_cache_fail(reason, ShaderRegexCacheFailureReason::BLOB_READ_FAILED);
	}

	for (i = 0; i < record->match_count; i++) {
		// The ShaderRegex groups are sorted and since the cached hash
		// already matched the map should be identical to when the
		// cache was made, so we can use that to find the matching
		// groups without having to do an expensive lookup by name:
		uint32_t match_id = match_pool[record->match_pool_index + i];
		group = shader_regex_group_index[match_id];

		LogDebug("ShaderRegexCache: %S %016I64x matches [%S]\n",
				shader_type, hash, group->ini_section.c_str());

		if (record->patched && tagline)
			tagline->append(std::wstring(L"[") + group->ini_section + std::wstring(L"]"));

		// The caller holds mCriticalSection for these global command-list edits.
		group->link_command_lists_and_filter_index(hash);
	}

	return record->patched ? ShaderRegexCache::PATCH : ShaderRegexCache::MATCH;
}

// Writes (or removes) the fixed-format "<hash>-<type>_regex.txt" export. This is
// a separate concern from the packed cache, so it lives outside save_meta: an
// early return in the cache bookkeeping must never be able to skip the export.
static void shader_regex_cache_export_fixed(UINT64 hash, const wchar_t *shader_type,
		bool patched, const std::string *asm_text, const std::wstring *tagline)
{
	wchar_t path[MAX_PATH];
	FILE *f = NULL;
	size_t suffix;

	suffix = swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_regex.", G->SHADER_CACHE_PATH, hash, shader_type);
	wcscpy_s(path+suffix, MAX_PATH-suffix, L"txt");

	if (!patched) {
		if (DeleteFile(path))
			LogDebug("Removed stale ShaderRegex assembly file %S\n", path);
		return;
	}

	wfopen_ensuring_access(&f, path, L"wb");
	if (!f) {
		LogWarning("Error storing ShaderRegex assembly to %S\n", path);
		return;
	}

	fprintf_s(f, "%S\n", tagline->c_str());
	fwrite(asm_text->c_str(), 1, asm_text->size(), f);

	fclose(f);
	LogDebug("Storing ShaderRegex assembly to %S\n", path);
}

// Two independent concerns, in two phases. The cache bookkeeping below commits
// the record in memory only - the disk write is deferred to flush() so a first
// analysis does not trigger a full metadata rewrite. The fixed-format export is
// a separate pre-existing concern, run from its own helper so no early return in
// the cache block can skip it.
void ShaderRegexCacheStore::save_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
		bool patched, std::string *asm_text, std::wstring *tagline)
{
	if (!G->SHADER_CACHE_PATH[0] || (!G->CACHE_SHADERS && !G->EXPORT_FIXED))
		return;

	// Export is independent of cache I/O failures.
	if (G->EXPORT_FIXED)
		shader_regex_cache_export_fixed(hash, shader_type, patched, asm_text, tagline);

	if (G->CACHE_SHADERS && !G->DISABLE_REGEX_CACHE) {
		ShaderRegexCacheRecord *record;
		ShaderStage stage;

		stage = shader_stage_from_label(shader_type);
		if (stage == ShaderStage::INVALID)
			return;

		if (!ensure_valid(true)) {
			LogWarning("ShaderRegexCache: save_meta %S %016I64x aborted - cache not usable\n", shader_type, hash);
			return;
		}

		// Bound the serialised metadata before growing either vector. The extra
		// record is a conservative allowance when this is an update.
		uint64_t required = sizeof(header) + (uint64_t)(records.size() + 1) * sizeof(ShaderRegexCacheRecord)
				+ ((uint64_t)match_pool.size() + match_ids->size()) * sizeof(uint32_t);
		if (required > BLOB_STORE_METADATA_MAX)
			return;
		record = find_or_insert_record(stage, hash);
		if (!record) {
			LogWarning("ShaderRegexCache: save_meta %S %016I64x failed to insert record\n", shader_type, hash);
			return;
		}

		// TODO: When we have a condition field in ShaderRegex: The evaluations
		// of *all* valid conditions (not just those matched) must qualify the
		// cache, either by encoding them in the filename or extending the
		// metadata format.

		// The old code deleted the stale .bin file here since the bytecode and
		// metadata were separate per-shader files. In the packed format the
		// record is the single source of truth: clearing patched (and/or the
		// blob_size of a PATCH record that never had its bytecode written) is
		// enough to stop the stale blob block from ever being loaded.
		record->patched = patched;

		// Same-generation re-analysis normally produces the same count: reuse
		// that slice rather than accumulating copies on every retry. New records
		// and changed-count repairs append; old files may already contain holes.
		if (record->match_count == match_ids->size()) {
			if (!match_ids->empty())
				std::copy(match_ids->begin(), match_ids->end(),
						match_pool.begin() + record->match_pool_index);
		} else {
			record->match_pool_index = (uint32_t)match_pool.size();
			record->match_count = (uint32_t)match_ids->size();
			match_pool.insert(match_pool.end(), match_ids->begin(), match_ids->end());
		}

		LogDebug("ShaderRegexCache: save_meta %S %016I64x record updated (matches=%u pool_total=%u patched=%d)\n",
				shader_type, hash, (unsigned)record->match_count, (unsigned)match_pool.size(), (int)patched);

		// Defer the disk write until the per-frame flush so we don't block
		// the render thread with a full metadata rewrite on every shader:
		metadata_dirty = true;
	}
}

void ShaderRegexCacheStore::save_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode)
{
	ShaderRegexCacheRecord *record;
	ShaderStage stage;

	if (!G->SHADER_CACHE_PATH[0] || !G->CACHE_SHADERS || G->DISABLE_REGEX_CACHE)
		return;
	if (!bytecode || bytecode->empty() || bytecode->size() > UINT32_MAX)
		return;

	stage = shader_stage_from_label(shader_type);
	if (stage == ShaderStage::INVALID)
		return;

	if (!ensure_valid(true)) {
		LogWarning("ShaderRegexCache: save_bin %S %016I64x aborted - cache not usable\n", shader_type, hash);
		return;
	}

	record = find_record(stage, hash);
	if (!record) {
		LogWarning("ShaderRegexCache: save_bin %S %016I64x no record (save_meta should run first)\n", shader_type, hash);
		return; // save_shader_regex_cache_meta() should have run before this
	}

	// The assembly can be re-run (e.g. after a config reload) and usually
	// produces identical bytecode. Skip the append in that common case so the
	// blob does not keep growing on every reload:
	if (record->blob_size == bytecode->size() && record->blob_size) {
		std::vector<byte> existing(bytecode->size());
		if (blob.read(record->blob_offset, record->blob_size, existing.data())
		 && existing == *bytecode)
			return;
	}

	// Validate before appending: load() refuses anything this rejects, so a bad
	// assemble would strand a dead block and cost a re-analysis every session.
	uint32_t dxbc_version;
	if (!shader_bytecode_dxbc_version(bytecode->data(), bytecode->size(), &dxbc_version)
	 || shader_stage_from_dxbc_type(dxbc_version >> 16) != stage
	 || !shader_bytecode_dxbc_instructions(bytecode->data(), bytecode->size())) {
		LogOverlayW(LOG_WARNING,
			L"ShaderRegex output for %016I64x-%ls failed bytecode validation; cache entry skipped\n",
			hash, shader_type);
		return;
	}

	uint64_t blob_offset;
	if (!blob.append(bytecode->data(), bytecode->size(), &blob_offset)) {
		LogWarning("ShaderRegexCache: save_bin %S %016I64x blob append failed (%Iu bytes)\n",
				shader_type, hash, bytecode->size());
		return;
	}

	record->blob_offset = blob_offset;
	record->blob_size = (uint32_t)bytecode->size();
	record->patched = 1;
	header.blob_size = blob.logical_end();

	LogDebug("ShaderRegexCache: save_bin %S %016I64x at blob offset %Iu (%u bytes, logical end %Iu)\n",
			shader_type, hash, (uint64_t)blob_offset, (unsigned)bytecode->size(), (uint64_t)header.blob_size);

	// Defer the disk write until the per-frame flush:
	metadata_dirty = true;
}

// ---- Public API ----

static void save_shader_regex_cache_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
		bool patched, std::string *asm_text, std::wstring *tagline)
{
	std::lock_guard<std::mutex> lock(shader_regex_cache_mutex);
	shader_regex_cache_store.save_meta(hash, shader_type, match_ids, patched, asm_text, tagline);
}

ShaderRegexCache load_shader_regex_cache(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode, std::wstring *tagline, ShaderRegexCacheFailureReason *reason)
{
	std::lock_guard<std::mutex> lock(shader_regex_cache_mutex);
	return shader_regex_cache_store.load(hash, shader_type, bytecode, tagline, reason);
}

void save_shader_regex_cache_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode)
{
	std::lock_guard<std::mutex> lock(shader_regex_cache_mutex);
	shader_regex_cache_store.save_bin(hash, shader_type, bytecode);
}

bool shader_regex_cache_flush()
{
	// is_dirty() is checked before taking the mutex, not after: a flush with
	// nothing queued is the overwhelmingly common case, and taking the cache
	// lock here would block the render thread behind whichever analysis is
	// currently inside ensure_valid() doing file I/O. The store's own flush()
	// re-checks metadata_dirty under the lock, so this is only an early-out.
	if (!shader_regex_cache_store.is_dirty())
		return true;

	std::lock_guard<std::mutex> lock(shader_regex_cache_mutex);
	return shader_regex_cache_store.flush();
}

bool get_shader_model_from_bytecode(const void* data, size_t size, std::string* out_model)
{
	if (!data || size < 32 || !out_model)
		return false;

	const uint8_t* buffer = static_cast<const uint8_t*>(data);

	// Validate DXBC header
	if (memcmp(buffer, "DXBC", 4) != 0)
		return false;

	const uint8_t* ptr = buffer + 4 + 16; // Skip FOURCC + hash

	// Read header fields
	if (ptr + 12 > buffer + size)
		return false;

	uint32_t one, totalSize, numChunks;
	memcpy(&one, ptr, 4); ptr += 4;
	memcpy(&totalSize, ptr, 4); ptr += 4;
	memcpy(&numChunks, ptr, 4); ptr += 4;

	if (numChunks == 0)
		return false;

	// Validate chunk table bounds
	if (ptr + numChunks * sizeof(uint32_t) > buffer + size)
		return false;

	const uint32_t* chunkOffsets = reinterpret_cast<const uint32_t*>(ptr);

	// Iterate chunks backwards (same as disassembler)
	for (int32_t i = (int32_t)numChunks - 1; i >= 0; --i)
	{
		uint32_t offset = chunkOffsets[i];

		if (offset + 12 > size)
			continue;

		const uint8_t* chunk = buffer + offset;

		// Look for shader code chunk
		if (memcmp(chunk, "SHEX", 4) != 0 && memcmp(chunk, "SHDR", 4) != 0)
			continue;

		// Version token is at +8
		uint32_t versionToken;
		memcpy(&versionToken, chunk + 8, 4);

		uint32_t type = (versionToken >> 16) & 0xFFFF;
		uint32_t major = (versionToken >> 4) & 0xF;
		uint32_t minor = (versionToken >> 0) & 0xF;

		// Map the DXBC version token's type field to the stage table. Unknown
		// types fall back to the table's "xx" row, matching the old default:
		const char *prefix = shader_stage_info_at(shader_stage_from_dxbc_type(type)).model_prefix;

		char buf[16];
		snprintf(buf, sizeof(buf), "%s_%u_%u", prefix, major, minor);

		*out_model = buf;
		return true;
	}

	return false;
}

// Process groups that do not have patches to apply. Those can be handled without disassembly.
void link_shader_regex_groups_without_patterns(const wchar_t* shader_type, std::string* shader_model, UINT64 hash, bool* decompilation_required)
{
	ShaderRegexGroups::iterator i;
	vector<uint32_t> match_ids;
	vector<ShaderRegexGroup*> match_groups;
	uint32_t j;

	for (i = shader_regex_groups.begin(), j = 0; i != shader_regex_groups.end(); i++, j++) {
		ShaderRegexGroup* group = &i->second;

		// Skip group without matching shader model.
		if (!group->shader_models.count(*shader_model)) {
			continue;
		}

		// Skip group with patterns. Those ones need txt to match.
		if (!group->patterns.empty()) {
			if (decompilation_required)
				*decompilation_required = true;
			continue;
		}

		match_ids.push_back(j);
		match_groups.push_back(group);

		LogDebug("ShaderRegex (no pattern): %S %016I64x matches [%S]\n", shader_type, hash, group->ini_section.c_str());
	}

	// If no ShaderRegEx requires decompilation, link CommandLists and update shader cache here instead of `apply_shader_regex_groups`. 
	if (decompilation_required && !*decompilation_required) {
		// Enable CommandList sections execution for this group.
		for (ShaderRegexGroup* group : match_groups) {
			group->link_command_lists_and_filter_index(hash);
		}
		// We save the cache metadata even if we didn't match anything. That
		// way we can skip checking for a match next time when we know there
		// won't be any. This only saves the metadata - the caller will use
		// save_shader_regex_cache_bin to save the assembled binary.
		save_shader_regex_cache_meta(hash, shader_type, &match_ids, false, nullptr, nullptr);
	}
}

bool apply_shader_regex_groups(std::string *asm_text, const wchar_t *shader_type, std::string *shader_model, UINT64 hash, std::wstring *tagline, bool cache_result)
{
	ShaderRegexGroups::iterator i;
	ShaderRegexGroup *group;
	bool patched = false;
	bool match, patch;
	vector<uint32_t> match_ids;
	uint32_t j;

	for (i = shader_regex_groups.begin(), j = 0; i != shader_regex_groups.end(); i++, j++) {
		group = &i->second;

		// Skip group without matching shader model.
		if (!group->shader_models.count(*shader_model)) {
			continue;
		}

		// Match/patch only ShaderRegEx with Pattern.
		if (!group->patterns.empty()) {
			// Run patch.
			group->apply_regex_patterns(asm_text, &match, &patch);
			if (!match)
				continue;

			LogDebug("ShaderRegex: %s %016I64x matches [%S]\n", shader_model->c_str(), hash, group->ini_section.c_str());
			patched = patched || patch;

			// Append section to patch sequence.
			if (patch && tagline)
				tagline->append(std::wstring(L"[") + group->ini_section + std::wstring(L"]"));
		}

		match_ids.push_back(j);

		// Enable CommandList sections execution for this group.
		group->link_command_lists_and_filter_index(hash);
	}

	// We save the cache metadata even if we didn't match anything. That
	// way we can skip checking for a match next time when we know there
	// won't be any. This only saves the metadata - the caller will use
	// save_shader_regex_cache_bin to save the assembled binary.
	//
	// Skipped for the hunting/dump path, which works from its own
	// disassembly of the original shader rather than this analysis.
	if (cache_result)
		save_shader_regex_cache_meta(hash, shader_type, &match_ids, patched, asm_text, tagline);

	return patched;
}