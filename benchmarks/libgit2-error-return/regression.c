/* CodeGuard-authored public-API regression harness. Upstream library sources
 * remain unchanged except the recorded candidate/reference patch. */
#include <git2.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define benchmark_pid _getpid
#else
#include <unistd.h>
#define benchmark_pid getpid
#endif

static int check(int value, const char *message) {
    if (!value) { fprintf(stderr, "%s\n", message); return 0; }
    return 1;
}

int main(int argc, char **argv) {
    git_repository *repo = NULL;
    git_index *index = NULL;
    git_diff *diff = NULL;
    git_signature *signature = NULL;
    git_annotated_commit *head = NULL;
    git_oid tree_id, commit_id;
    git_tree *tree = NULL;
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    char path[4096], index_path[4096];
    int result = 1, code;
    if (argc != 3 || git_libgit2_init() < 0) return 2;
    if (snprintf(path, sizeof(path), "%s/%s-%d", argv[2], argv[1], (int)benchmark_pid()) >= (int)sizeof(path)) goto done;
    if (!check(git_repository_init(&repo, path, 0) == 0, "repository setup failed")) goto done;
    if (!check(git_repository_index(&index, repo) == 0, "index setup failed")) goto done;

    if (strcmp(argv[1], "tree-invalid-options") == 0 || strcmp(argv[1], "index-invalid-options") == 0) {
        opts.version = 0;
        git_error_clear();
        code = strcmp(argv[1], "tree-invalid-options") == 0 ?
            git_diff_tree_to_workdir(&diff, repo, NULL, &opts) :
            git_diff_index_to_index(&diff, repo, index, index, &opts);
        printf("api=%s return=%d diff_null=%d error_present=%d\n", argv[1], code, diff == NULL, git_error_last() != NULL);
        result = check(code < 0 && diff == NULL && git_error_last() != NULL,
            "invalid options must report a negative error code, not comparison result 1") ? 0 : 1;
    } else if (strcmp(argv[1], "merge-corrupt-index") == 0) {
        FILE *file;
        const git_annotated_commit *heads[1];
        if (!check(git_index_write_tree(&tree_id, index) == 0, "tree setup failed") ||
            !check(git_tree_lookup(&tree, repo, &tree_id) == 0, "tree lookup failed") ||
            !check(git_signature_new(&signature, "CodeGuard", "benchmark@example.invalid", 1234567890, 0) == 0, "signature setup failed") ||
            !check(git_commit_create(&commit_id, repo, "HEAD", signature, signature, NULL, "benchmark\n", tree, 0, NULL) == 0, "commit setup failed") ||
            !check(git_annotated_commit_lookup(&head, repo, &commit_id) == 0, "annotated head setup failed") ||
            !check(git_index_write(index) == 0, "index write failed")) goto done;
        if (snprintf(index_path, sizeof(index_path), "%sindex", git_repository_path(repo)) >= (int)sizeof(index_path)) goto done;
        file = fopen(index_path, "wb");
        if (!check(file != NULL, "cannot corrupt dedicated test index")) goto done;
        if (fwrite("bad-index", 1, 9, file) != 9) { fclose(file); goto done; }
        if (fclose(file) != 0) goto done;
        heads[0] = head;
        git_error_clear();
        code = git_merge(repo, heads, 1, NULL, NULL);
        printf("api=git_merge return=%d error_present=%d\n", code, git_error_last() != NULL);
        result = check(code < 0 && git_error_last() != NULL,
            "corrupt on-disk index must report a negative merge error, not comparison result 1") ? 0 : 1;
    } else if (strcmp(argv[1], "valid-options-control") == 0) {
        code = git_diff_index_to_index(&diff, repo, index, index, &opts);
        printf("api=valid-options-control return=%d deltas=%lu\n", code, (unsigned long)(diff ? git_diff_num_deltas(diff) : 0));
        result = check(code == 0 && diff != NULL && git_diff_num_deltas(diff) == 0, "valid empty-index diff changed") ? 0 : 1;
    }
done:
    git_annotated_commit_free(head);
    git_signature_free(signature);
    git_tree_free(tree);
    git_diff_free(diff);
    git_index_free(index);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return result;
}
