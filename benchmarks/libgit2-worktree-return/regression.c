/* CodeGuard-authored Windows public-API regressions; dedicated repositories only. */
#include <git2.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#include <direct.h>
#include <windows.h>
#define benchmark_pid _getpid
#else
#include <unistd.h>
#define benchmark_pid getpid
#endif
#define REQUIRE(expr) do { if (!(expr)) { const git_error *e=git_error_last(); fprintf(stderr,"setup/check failed: %s (%s)\n",#expr,e?e->message:""); goto done; } } while(0)
static int write_text(const char *dir,const char *name,const char *text) {
    char path[4096];FILE *file;int written,closed;
    if(snprintf(path,sizeof(path),"%s/%s",dir,name)>=(int)sizeof(path))return -1;
    file=fopen(path,"wb");if(!file)return -1;
    written=fputs(text,file);closed=fclose(file);return written<0 || closed!=0?-1:0;
}
int main(int argc,char **argv) {
    git_repository *repo=NULL;git_index *index=NULL;git_tree *tree=NULL;
    git_signature *signature=NULL;git_reference *branch=NULL,*renamed=NULL,*head=NULL;
    git_worktree *worktree=NULL;git_strarray worktrees={0};
    git_oid tree_id,commit_id;int result=2,code,head_locked,metadata_locked;
    char path[4096],repo_path[4096],admin[4096],link[4096];
#ifdef _WIN32
    HANDLE metadata_handle=INVALID_HANDLE_VALUE;
#endif
    if(argc!=3 || git_libgit2_init()<0)return 2;
    head_locked=strcmp(argv[1],"head-lock")==0;
    metadata_locked=strcmp(argv[1],"worktree-read-lock")==0;
    REQUIRE(head_locked || metadata_locked || strcmp(argv[1],"control")==0);
    REQUIRE(snprintf(repo_path,sizeof(repo_path),"%s-%d",argv[2],(int)benchmark_pid())<(int)sizeof(repo_path));
    REQUIRE(git_repository_init(&repo,repo_path,0)==0);
    REQUIRE(git_repository_index(&index,repo)==0);
    REQUIRE(git_index_write_tree(&tree_id,index)==0);
    REQUIRE(git_tree_lookup(&tree,repo,&tree_id)==0);
    REQUIRE(git_signature_new(&signature,"CodeGuard","benchmark@example.invalid",1234567890,0)==0);
    REQUIRE(git_commit_create(&commit_id,repo,"HEAD",signature,signature,NULL,"benchmark\n",tree,0,NULL)==0);
    REQUIRE(git_repository_head(&branch,repo)==0);
    if(head_locked)REQUIRE(write_text(git_repository_path(repo),"HEAD.lock","CodeGuard test-owned lock\n")==0);
    if(metadata_locked) {
#ifdef _WIN32
        REQUIRE(snprintf(admin,sizeof(admin),"%sworktrees",git_repository_path(repo))<(int)sizeof(admin));
        REQUIRE(_mkdir(admin)==0);
        REQUIRE(snprintf(admin,sizeof(admin),"%sworktrees/stale",git_repository_path(repo))<(int)sizeof(admin));
        REQUIRE(_mkdir(admin)==0);
        REQUIRE(snprintf(link,sizeof(link),"%s/missing/.git\n",repo_path)<(int)sizeof(link));
        REQUIRE(write_text(admin,"commondir","../..\n")==0);
        REQUIRE(write_text(admin,"gitdir",link)==0);
        REQUIRE(write_text(admin,"HEAD","ref: refs/heads/missing\n")==0);
        REQUIRE(snprintf(path,sizeof(path),"%s/commondir",admin)<(int)sizeof(path));
        metadata_handle=CreateFileA(path,GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        REQUIRE(metadata_handle!=INVALID_HANDLE_VALUE);
        REQUIRE(git_worktree_list(&worktrees,repo)==0);
        REQUIRE(worktrees.count==1 && strcmp(worktrees.strings[0],"stale")==0);
        code=git_worktree_lookup(&worktree,repo,"stale");
        printf("worktree_list=%u lookup=%d\n",(unsigned)worktrees.count,code);
        REQUIRE(code==-1);
#else
        fprintf(stderr,"worktree-read-lock requires Windows sharing semantics\n");goto done;
#endif
    }
    git_error_clear();
    code=git_reference_rename(&renamed,branch,"refs/heads/renamed",0,"CodeGuard benchmark rename");
    printf("case=%s api=git_reference_rename return=%d error_present=%d\n",argv[1],code,git_error_last()!=NULL);
    if(head_locked)result=(code==GIT_ELOCKED && git_error_last()!=NULL)?0:1;
    else {
        REQUIRE(git_reference_lookup(&head,repo,"HEAD")==0);
        REQUIRE(git_reference_type(head)==GIT_REFERENCE_SYMBOLIC);
        printf("head_target=%s\n",git_reference_symbolic_target(head));
        result=(code==(metadata_locked?-1:0) && strcmp(git_reference_symbolic_target(head),"refs/heads/renamed")==0)?0:1;
    }
done:
#ifdef _WIN32
    if(metadata_handle!=INVALID_HANDLE_VALUE)CloseHandle(metadata_handle);
#endif
    git_worktree_free(worktree);git_strarray_dispose(&worktrees);
    git_reference_free(head);git_reference_free(renamed);git_reference_free(branch);
    git_signature_free(signature);git_tree_free(tree);git_index_free(index);
    git_repository_free(repo);git_libgit2_shutdown();return result;
}
