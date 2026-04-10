/*
按照题目要求，需要完成的函数是mm_init,mm_malloc,mm_free,mm_realloc
为了调试，还要写一个mm_check函数，来检查堆的一致性（即检查堆在进行一些操作之后是否
还符合你一开始对其的一些设定)
mm_init用于完成对堆的一些初始结构设定，失败则返回-1，成功则返回0
mm_malloc，返回一个指向至少size大小的成功分配好的区域的指针，该区域位于堆之内
这里面很重要的一个问题就是如何比较高效率的找到一块合适的区域，以及如果找的这块区域大了，剩下的部分该如何处理
mm_free，释放指针ptr指向的区域，当然ptr指向的区域必须是之前通过malloc或者realloc分配的且尚未释放的
当然，free的时候必须要考虑到是否需要合并的问题
mm_realloc，接受一个指针和一个size
若指针为null，就等价于调用mm_malloc(size)
如果size=0，则等价于调用mm_free(ptr)
否则，就是想让ptr指向的那块区域改成size大小，并返回一个（可能）新的地址，看情况
其中，一个比较主要的需要解决的问题怎么管理空闲块，以便在malloc的时候能比较快的找到适合的空闲块
书上提供的方法有
隐式空闲链表，显式空闲链表，简单分离储存，分离适配
按理来说，分离适配的效率是最好的，所以本项目将采用分离适配的方法
概括来说，就是要维护一组空闲链表的数组，每个空闲链表与一个大小类相对应
就是每个数组元素都是指针，指向对应的大小类的双向链表的头（由于后续可能涉及到插入，感觉可以用循环双向链表，可以快速插入到末尾）
当然，由于题目要求不能创建全局数组，所以这个数组是在Init的时候，
放在堆上的一块区域
另外，由于区域内需要放置prev,next这些内容，所以应该设定一个最小块大小
此外，书上有提到，在分配的块不为空的时候，仍然维持脚部可能带来不必要的内存开销
解决的办法是对空闲块保留脚部，而对已分配块则去掉脚部，只用空闲位记录是否已分配，
记得之前讲过，表示地址的数字，最后3位一定是0，其中，最后一位已经用来表示当前块是否分配了
还有剩下的位，可以用来表示前一块是否已分配

分配块结构：前32位放置块大小，低3位中，最后一位记录当前块是否分配，倒数第二位记录前一块是否分配
接下来放置有效荷载部分，最后放置可能的填充部分，需要注意，每一块都是8字节（双word)对齐的
空闲块结构：头部放置块大小和当前块，前一块是否分配情况，尾部与头部一样，还要两个word的空间，一个用来放前空闲块位置，一个用来放后空闲块位置
也就是说，我们的分配块最小是4word
空闲列表组织：在init堆的时候，分配一块区域，给到一个指针数组，每个元素对应一个组（比如16-31，32-63字节大小，64-127,128-255等等），按照计算和案例中实际分配的最大块大小情况，一共16组，最后一组是524288-无穷
在管理空闲列表的时候，用首次适配，后进先出原则，也就是说，找空闲块的时候，进到一个组找到的第一个可行的块作为分配块，然后处理（可能有的）剩余块，把新的空闲块放到链表的头部，free的时候也是同理，
其中，这里的双向链表我们使用线性的双向链表
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

/*********************************************************
 * NOTE TO STUDENTS: Before you do anything else, please
 * provide your team information in the following struct.
 ********************************************************/
team_t team = {
    /* Team name */
    "nancy_team",
    /* First member's full name */
    "nancy",
    /* First member's email address */
    "yeah",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""
};

/*8字节对齐*/
#define ALIGNMENT 8
#define WSIZE 4
#define DSIZE 8
#define CHUNKSIZE (1 << 6)
/*找到最接近size且是8的倍数的数字*/
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~0x7)
//就是8
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))
//合并size和alloc信息
#define PACK(size,alloc) ((size) | (alloc))
//在p处读一个word
#define GET(p) (*(unsigned int *) (p))
//在p处放置一个word
#define PUT(p, val) (*(unsigned int *)(p) = (unsigned int)(val))
//从头部得到size信息
#define GET_SIZE(p) (GET(p) & ~0x7)
//得到当前块分配信息
#define GET_ALLOC(p) (GET(p) & 0x1)
//得到前一块的分配信息
#define GET_PRE_ALLOC(p) ((GET(p) >> 1) & 0x1) //这样输出的是0或1
//已知bp位置，算出头部位置
#define HDRP(bp) ((char*)(bp) - WSIZE)
//已知bp位置，算出脚部位置（这应该是对于空闲块）
#define FTRP(bp) ((char*)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)
//已知bp,算出放prev指针的位置
#define PREV_FREE(bp) ((char*)(bp))
//已知bp,算出放next指针的位置
#define NEXT_FREE(bp) ((char*)(bp) + WSIZE)
//已知当前bp，跳到下一块的bp处
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char*)(bp) - WSIZE)))
//跳到前一块bp处(当然，也是针对有脚部的情况)
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char*)(bp) - DSIZE)))
//分离链表数组大小
#define LIST_COUNT 16
/* 调试开关：开发时设为 1，提交前设为 0 */
#define CHECKHEAP(verbose) do { \
    printf("--- Checking heap at line %d ---\n", __LINE__); \
    if (!mm_check()) { \
        printf("Heap error at line %d\n", __LINE__); \
        exit(1); \
    } \
} while (0)
#define MAX(x, y) ((x) > (y) ? (x) : (y))
static char *heap_listp;
static char *free_list_start = NULL;
static void *extend_heap(size_t words);
int find_group(size_t size);
char *find_list_head(int index);
char* find_available_block(size_t size);
void add_to_list(size_t size, char* bp);
void remove_from_list(size_t size, char* bp);
static char *coalesce (char*bp);
int mm_check(void);
/* 
 * mm_init - initialize the malloc package.
 按照书中的提示，初始化的时候需要一个序言块（2word），一个结尾块（1word),一个填充块（1word）
 */
int mm_init(void)
{
    size_t init_size = (LIST_COUNT + 4)* WSIZE;
    char *p = mem_sbrk(init_size);
    if (p == (void *)-1) return -1;
    //初始化链表数组
    for (int i = 0; i < LIST_COUNT; i++) {
        PUT(p + WSIZE+(i*WSIZE),(unsigned int)NULL);//因为第一个是填充位
    }
    free_list_start = p + WSIZE;
    char *prologue_base = p + WSIZE + LIST_COUNT * WSIZE;//序言块的起始位
    PUT(p , 0);
    PUT(prologue_base, PACK(DSIZE, 3));      
    PUT(prologue_base + WSIZE, PACK(DSIZE, 3));  
    //之所以把结尾块的pre_alloc位记为1，是因为序言块一定是已分配的，
    //结尾块后面虽然没有内容，但是他也是需要承担一个普通头部的功能，
    //即正确记录前块是否分配
    PUT(prologue_base + (2 * WSIZE), PACK(0, 3)); 
    heap_listp = prologue_base + WSIZE;
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL) return -1;
    return 0;
}
static void *extend_heap(size_t words) {
    char *bp;
    size_t size;

    /* 向上舍入到双字对齐 */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    if (size < 16) size = 16;
    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;
    //注意，bp此时指向的是旧结尾块的末尾处，得到了旧的最后一块的分配信息
    int is_old_last_alloc = GET_PRE_ALLOC(HDRP(bp));
    PUT(HDRP(bp), PACK(size, is_old_last_alloc << 1));         /* 新空闲块 Header */
    PUT(FTRP(bp), PACK(size, is_old_last_alloc << 1));         /* 新空闲块 Footer */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* 新的结尾块 Epilogue */
    add_to_list(size,bp);
    if(!is_old_last_alloc) { //如果原来的最后一块为空
        char* old_bp = bp;
        size_t old_size = size;
        char* old_pre_bp = PREV_BLKP(bp);
        size_t old_pre_size = GET_SIZE(HDRP(PREV_BLKP(bp)));
        size += old_pre_size ;
        //合并后的头部，当前块分配信息为0
        //pre_alloc为1
        PUT(HDRP(PREV_BLKP(bp)),PACK(size,2));
        //脚部同理
        PUT(FTRP(bp),PACK(size,2));
        bp = PREV_BLKP(bp);
        remove_from_list(old_size,old_bp);//移除旧块
        remove_from_list(old_pre_size,old_pre_bp); //移除旧的前块
        add_to_list(size,bp);//添加新的合并块
    }
    return bp; 
}
/* 
malloc,分配一个大小为size的空间，并返回一个指向bp的指针
首先需要在空闲块中寻找合适的可以用作分配的块，
如果能找到一个块，则分配完之后还需要考虑有没有碎片，
这里要注意，如果碎片大于等于4word,我们把他正常分割，加入对应的空闲块链表，
但是如果小于4word,那么我们就不分割了，直接把这一小块加到malloc的空间里面去（实际上也最多浪费2word)
如果空闲链表中没有合适的块，则extend(CHUNKSIZE/WSIZE)，extend之后记得要合并
然后再执行分配函数，这里有需要extend多次的可能，不过直接调用mm_malloc(size)就行，一次调用空间不够会自动继续扩展
 */
void *mm_malloc(size_t size)
{
    //CHECKHEAP(1);
    int realsize = ALIGN(size + 4); //加上了头部
    if (realsize < 16) {
        realsize = 16; //如果算出来需要分配的空间小于16，则取16
    }
    char* bp = find_available_block(realsize);
    if(bp != NULL) { //如果找到了合适的块
    int free_block_size = GET_SIZE(HDRP(bp)); //供分配空闲块大小
    size_t remain_size = free_block_size - realsize;
    if (remain_size < 16) { //分出来碎片小于16,则不分碎片
        PUT(HDRP(bp), PACK(free_block_size,3));
        //PUT(FTRP(bp), PACK(free_block_size,1));
        //更新下一块block记录前一块是否分配的位
        char* next_head = HDRP(NEXT_BLKP(bp));
        PUT(next_head,PACK(GET(next_head),0x2));
        // if (!GET_ALLOC(next_head)) { //如果下一块是空的，那么还要更新尾部信息
        //     PUT(FTRP(bp),GET(next_head));
        // }
        //删掉是因为找到的这块bp原来分配前就是空的，那么他的下一块不可能为空
        remove_from_list(free_block_size,bp);//从freelist里面删掉这一块
        //CHECKHEAP(1);
        return bp;
    }
    else { //需要分出来一块碎片
        char* next_foot = FTRP(bp); //这是修改前的块的脚部位置，也是分割后的空闲块的脚部位置
        //这里不需要继承原来块的pre_alloc信息是因为原来块是空，则原来块的pre_alloc一定是1
        PUT(HDRP(bp), PACK(realsize,3));
        remove_from_list(free_block_size,bp);//从freelist里面删掉原本这一块
        size_t fragment_size = remain_size ;
        char* next_head = HDRP(NEXT_BLKP(bp));
        PUT(next_head,PACK(remain_size,2));//当前块未分配，前一块已分配
        PUT(next_foot,PACK(remain_size,2));
        char* next_bp = NEXT_BLKP(bp); //空闲块的bp
        add_to_list(fragment_size,next_bp);//把碎片加入链表
        //CHECKHEAP(1);
        return bp;
    }
    }
    else //接下来考虑在空闲块链表中没有找到合适的free块的情况
    {
        if (extend_heap(realsize / WSIZE) == NULL)
        {
            return NULL;
        }
        //CHECKHEAP(1);
        return mm_malloc(size);
    }
}

/*
先记得处理NULL
释放给定的一块区域，把head和foot的分配位设为0,记得要继承该块的pre_alloc信息
更改该块后一块的pre_alloc标记
然后直接coalesce就可以了(易得，不需要再处理pre_alloc标记问题)

 */
void mm_free(void *ptr)
{    
    //CHECKHEAP(1);
    if (ptr == NULL) {
        return;
    }
    size_t size = GET_SIZE(HDRP(ptr));
    int pre_alloc = GET_PRE_ALLOC(HDRP(ptr));
    PUT(HDRP(ptr),PACK(size,pre_alloc*2));
    PUT(FTRP(ptr),PACK(size,pre_alloc*2));
    //更改free块的后一块的头部有关pre_alloc的信息为0
    PUT(HDRP(NEXT_BLKP(ptr)),GET(HDRP(NEXT_BLKP(ptr))) & ~0x2);
    //不需要更改尾部，因为那是coalesce的工作
    //add_to_list(size,ptr);//先把free掉的这一块加入链表
    //CHECKHEAP(1);
    coalesce(ptr);
    //CHECKHEAP(1);
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
有一个重要的原则;无论怎么变，
旧块中从 0 到 min(old_size, new_size) 的内容必须原封不动地保留。
分为以下几种情况：
先将size转化为8的倍数（小于16记为16）asize
若asize <= oldsize,则先看oldsize-asize相差多少，若相差小于16，
则剩余的块不分割，那么相当于一切没有变化
如果相差大于等于16，说明剩余的块还要分割，那么需要重新设立边界，并把新的碎片加入空闲链表
如果asize > oldsize,就检查next是否为空，如果为空，再看next_size+oldsize是否大于等于asize
如果大于等于，说明可以原地扩容，原先区域的数据也不需要挪动
如果上面的情况不符合，还需要看当前块是否已经是堆尾部，如果是，则可以直接调用extend_heap，申请需要的差值，
这样的话，数据同样不需要挪动，ptr也在原地，只是要更改一下头部size信息即可
如果上面的情况都不符合，那么说明必须要挪动位置来分配了
new_ptr = malloc(size)
memcpy(new_ptr, old_ptr, old_size - WSIZE),拷贝数据
free(old_ptr)
然后返回new_ptr
 */
void *mm_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return mm_malloc(size);
    } else if (size == 0){
        mm_free(ptr);
        return NULL;
    }
    size_t new_size = ALIGN(size + WSIZE);
    size_t old_size = GET_SIZE(HDRP(ptr));
    if (new_size < 16) {
        new_size = 16;
    }
    int fragment = old_size - new_size;
    if (fragment >= 0) { //新size小于等于原size
        if (fragment < 16) { //碎片过小，不分割
            return ptr;
        } 
        else { //新size小于原size且分割碎片
            int pre_alloc = GET_PRE_ALLOC(HDRP(ptr));
            int next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(ptr)));
            //更新新块的头部信息，size为new_size,alloc为1，pre_alloc为原信息
            PUT(HDRP(ptr),PACK(new_size,1+2*pre_alloc));
            //更新新碎片的头部信息，alloc为0，pre_alloc为1，size为fragment
            PUT(HDRP(NEXT_BLKP(ptr)),PACK(fragment,2));
            PUT(FTRP(NEXT_BLKP(ptr)),PACK(fragment,2));
            //记得存在一种原来块的后一块为空，那么新的碎片和原来空的next块可以合并的情况
            if (next_alloc == 0) {
                coalesce(NEXT_BLKP(ptr));
                return ptr;
            }
            add_to_list(fragment,NEXT_BLKP(ptr));
            //记得还要更新碎片的next块的pre_alloc信息
            char *next_next_block = NEXT_BLKP(NEXT_BLKP(ptr));
            //把next_next块的pre_alloc信息改成0
            PUT(HDRP(next_next_block), GET(HDRP(next_next_block)) & ~0x2);
            return ptr;
        }

    }
    else { //新size大于原size
        int is_next_free = GET_ALLOC(HDRP(NEXT_BLKP(ptr)));
        int is_prev_free = GET_PRE_ALLOC(HDRP(ptr));
        if (is_next_free == 0) { //下一块为空
            size_t next_size = GET_SIZE(HDRP(NEXT_BLKP(ptr)));
            size_t add_size = old_size + next_size;
            int fragment1 = add_size - new_size; //两块加在一起的大小与新size之差
            if (fragment1 >= 0) {
                if (fragment1 < 16) { //相当于直接把后面那块空闲的合并到这个malloc块中
                int pre_alloc = GET_PRE_ALLOC(HDRP(ptr));
                char* old_next = NEXT_BLKP(ptr);
                //把空的next块从空闲链表删除
                remove_from_list(next_size,old_next);
                //更改新块的头部信息
                PUT(HDRP(ptr),PACK(add_size,1+2*pre_alloc));
                //还要更改原next空闲块后一块的pre_alloc信息
                //新块的next块的pre_alloc为1
                PUT(HDRP(NEXT_BLKP(ptr)),GET(HDRP(NEXT_BLKP(ptr))) | 0x2);
                return ptr;
            } else { //两块加在一起与所需size之差大于16，说明还可以分出一块碎片
                int pre_alloc = GET_PRE_ALLOC(HDRP(ptr));
                char* old_next = NEXT_BLKP(ptr);
                //把空的next块从空闲链表删除
                remove_from_list(next_size,old_next);
                PUT(HDRP(ptr),PACK(new_size,1+2*pre_alloc));
                //更新新的碎片块的头部信息，大小为fragment1,alloc为0，pre_alloc为1
                PUT(HDRP(NEXT_BLKP(ptr)),PACK(fragment1,2));
                //尾部同理
                PUT(FTRP(NEXT_BLKP(ptr)),PACK(fragment1,2));
                //把新的碎片块加到空闲链表
                add_to_list(fragment1,NEXT_BLKP(ptr));
                return ptr;
                }
            } else { //虽然下一块为空，但加起来空间不够
                //注意，这里还存在一种可能，即Next块的空间虽然不够，但是已经是堆的最后一块了
                if (GET_SIZE(HDRP(NEXT_BLKP(NEXT_BLKP(ptr)))) == 0) { //当前块的下一块是堆的最后一块
                    size_t needed_space = (new_size - add_size)*2;
                    extend_heap(needed_space /WSIZE);
                    return mm_realloc(ptr,size);
                }
                char* new_ptr = mm_malloc(size);
                memcpy(new_ptr , ptr , old_size - WSIZE);//把原来的内容复制过去
                mm_free(ptr);
                return new_ptr;
            }
            
        } 
        else if (is_prev_free == 0) {
            size_t prev_size = GET_SIZE(HDRP(PREV_BLKP(ptr)));
            size_t add_size = old_size + prev_size;
            int fragment2 = add_size - new_size;
            if (fragment2 >= 0) {
                char* old_ptr = ptr;
                char* new_ptr = PREV_BLKP(ptr);
                int pre_alloc = GET_PRE_ALLOC(HDRP(new_ptr));
                ptr = new_ptr;
                remove_from_list(prev_size, ptr);
                memmove(new_ptr, old_ptr, old_size - WSIZE); 
                if (fragment2 < 16) { //直接把前面的块合并到这一块
                    //更改新块的头部信息
                    PUT(HDRP(ptr),PACK(add_size,1+2*pre_alloc));
                    return ptr;
                } else { //两块加在一起与所需size之差大于16，说明还可以分出一块碎片
                    PUT(HDRP(ptr),PACK(new_size,1+2*pre_alloc));
                    //更新新的碎片块的头部信息，大小为fragment2,alloc为0，pre_alloc为1
                    PUT(HDRP(NEXT_BLKP(ptr)),PACK(fragment2,2));
                    //尾部同理
                    PUT(FTRP(NEXT_BLKP(ptr)),PACK(fragment2,2));
                    //把新的碎片块加到空闲链表
                    add_to_list(fragment2,NEXT_BLKP(ptr));
                    //还记得要更改原块的next块,（即现ptr的next,next块）的pre_alloc信息为0
                    PUT(HDRP(NEXT_BLKP(NEXT_BLKP(ptr))),GET(HDRP(NEXT_BLKP(NEXT_BLKP(ptr)))) & ~0x2);
                    return ptr;
                }
            }
            else { //虽然前一块为空，但是加起来空间不够
                char* new_ptr = mm_malloc(size);
                memmove(new_ptr , ptr , old_size - WSIZE);//把原来的内容复制过去
                mm_free(ptr);
                return new_ptr;
            }
        }
        else if (GET_SIZE(HDRP(NEXT_BLKP(ptr))) == 0){ 
            //新size大于oldsize,且next不是free
            //注意这里可能有一种特殊情况，就是当前realloc这一块已经是堆尾部
            //堆结束块的特征就是size为0，而alloc为1
            size_t needed_space = (new_size - old_size)*2;
            //扩充所需的空间
            extend_heap(needed_space /WSIZE);
            //现在已经在next造出了一块足够大的空闲块
            //只需要再次调用即可
            return mm_realloc(ptr,size);
        } else {
            //新size大于oldsize,且next不是free
            //且realloc这一块不是堆尾
            //只能重新找地方
            char* new_ptr = mm_malloc(size);
            memmove(new_ptr , ptr , old_size - WSIZE);//把原来的内容复制过去
            mm_free(ptr);
            return new_ptr;
        }
    }
}

//给一个size,返回应该去哪个组寻找
int find_group(size_t size) {//这里我实际上默认size是大于等于16且8字节对齐的
    //16到31是第0组
    int size1 = size >> 4;
    int index = 0;
    while(size1 > 1 && index < LIST_COUNT - 1) {
        size1 = size1 >> 1;
        index += 1;
    }
    return index;
}
char *find_list_head(int index){ //找到链表头中储存的地址
    return (char *)GET(free_list_start + index * WSIZE);
}

//首次适配法
char* find_available_block_first(size_t size) {
    int index = find_group(size);
    char *list_head = find_list_head(index);
    //顺着头一个个往下找，如果该链表找不到，则到下一个更大的链表组寻找，但是注意链表组只有16个
    while(index < 16) { //最多只有16组
        char* bp = list_head;
        while(bp != NULL) {
            if(GET_SIZE(HDRP(bp)) >= size) { //首次适配法，找到了合适的块
                return bp;
            }
            bp = (char *)GET(NEXT_FREE(bp));//next指针指向的地方
        }
        index+= 1;
        list_head = find_list_head(index);//找下一个链表组
    }
    return NULL;//如果都没找到，就从这里退出了
}

//最佳适配法
char* find_available_block(size_t size) {
    int index = find_group(size);
    char *best_bp = NULL; 
    size_t min_size = 0xffffffff;
    while(index < 16) {
        char *bp = find_list_head(index); 
        
        while(bp != NULL) {
            size_t currentsize = GET_SIZE(HDRP(bp));
            if (currentsize >= size) {
                if (currentsize == size) return bp; 
                if (currentsize < min_size) {
                    best_bp = bp;
                    min_size = currentsize;
                }
            }
            bp = (char *)GET(NEXT_FREE(bp));
        }
        if (best_bp != NULL) {
            return best_bp;
        }
        index++; 
    }
    return NULL;
}

void add_to_list(size_t size, char* bp) { //把大小为size,位置为bp的块加入空闲链表
    int index = find_group(size);
    char* old_head = find_list_head(index);
    PUT(NEXT_FREE(bp) , old_head);
    PUT(PREV_FREE(bp) , (unsigned int)NULL);
    if(old_head != NULL){//只有在原先头部存在的时候，才需要更新原先头部的prev指针
        PUT(PREV_FREE(old_head) , bp);
    }
    //把bp变成新的头部
    PUT(free_list_start + index * WSIZE, (unsigned int)bp);
}

void remove_from_list(size_t size, char* bp) { //把大小为size,位置为bp的块从空闲链表删除
    int index = find_group(size);
    char* prev = (char *)GET(PREV_FREE(bp)); //前一个链表的位置
    char* next = (char *)GET(NEXT_FREE(bp)); //后一个链表的位置
    if(prev == NULL) {
        PUT(free_list_start + index * WSIZE, (unsigned int)next);
        //PUT(PREV_FREE(next),(unsigned int)NULL);//现在next变成了新的链表头
        //删掉是因为next不一定有，这里的逻辑到后面if next != null一起处理了
    } else{
        PUT(NEXT_FREE(prev), (unsigned int)next);
    }
    if(next != NULL) { //next不是空，则更新其prev
        PUT(PREV_FREE(next), (unsigned int)prev);
    }
}

static char *coalesce(char* bp) { //在free或者extend的情况下使用，合并空闲块
    //在这个函数中，我们要实现把块从链表删除/从链表插入块的操作
    //为了优化性能，选择在合并之前先不把原本块加入链表
    size_t size = GET_SIZE(HDRP(bp));//当前块尺寸
    int pre_alloc = GET_PRE_ALLOC(HDRP(bp));
    int next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    if (pre_alloc && next_alloc) {
        add_to_list(size,bp);
        return bp;
    }
    else if (!pre_alloc && next_alloc) {
        // char* old_bp = bp;
        // size_t old_size = size;
        char* old_pre_bp = PREV_BLKP(bp);
        size_t old_pre_size = GET_SIZE(HDRP(PREV_BLKP(bp)));
        size += old_pre_size ;
        //int pre_pre_alloc = GET_PRE_ALLOC(HDRP(PREV_BLKP(bp)));
        //之所以删掉这一行是因为如果新free这一块的前一块是free，那么pre的pre不可能还是free,不然就不符合规则了
        //合并后的头部，当前块分配信息为0
        //记得合并后那一块的pre_alloc信息是1
        //因为如果原先的pre块未分配，那么pre的pre一定是已分配
        PUT(HDRP(PREV_BLKP(bp)),PACK(size,2));
        //脚部同理
        PUT(FTRP(bp),PACK(size,2));
        bp = PREV_BLKP(bp);
        //remove_from_list(old_size,old_bp);//移除旧块
        remove_from_list(old_pre_size,old_pre_bp); //移除旧的前块
        add_to_list(size,bp);//添加新的合并块
        return bp;
    }
    else if (pre_alloc && !next_alloc) {
        // char* old_bp = bp;
        // size_t old_size = size;
        char* old_next_bp = NEXT_BLKP(bp);
        size_t old_next_size = GET_SIZE(HDRP(NEXT_BLKP(bp)));
        size += old_next_size;
        int pre_alloc = GET_PRE_ALLOC(HDRP(bp));//得到当前块的pre_alloc信息
        PUT(HDRP(bp),PACK(size,pre_alloc*2));//合并后的头部，当前块分配信息为0，但记得要继承原先当前块的pre_alloc信息
        //因为已经更新了头部size信息，所以可以直接用FTRP找到脚部
        PUT(FTRP(bp),PACK(size,pre_alloc*2));
        //remove_from_list(old_size,old_bp);//移除旧块
        remove_from_list(old_next_size,old_next_bp);//移除旧的next块
        add_to_list(size,bp);//添加新的合并块
        return bp;
    }
    else {
        // char* old_bp = bp;
        // size_t old_size = size;
        char* old_pre_bp = PREV_BLKP(bp);
        size_t old_pre_size = GET_SIZE(HDRP(PREV_BLKP(bp)));
        char* old_next_bp = NEXT_BLKP(bp);
        size_t old_next_size = GET_SIZE(HDRP(NEXT_BLKP(bp)));
        size += old_pre_size+old_next_size;
        int pre_pre_alloc = GET_PRE_ALLOC(HDRP(PREV_BLKP(bp)));
        //更新新的头部，脚部的信息
        PUT(HDRP(PREV_BLKP(bp)),PACK(size,pre_pre_alloc*2));
        PUT(FTRP(NEXT_BLKP(bp)),PACK(size,pre_pre_alloc*2));
        bp = PREV_BLKP(bp);
        //remove_from_list(old_size,old_bp);//移除旧块
        remove_from_list(old_pre_size,old_pre_bp); //移除旧的前块
        remove_from_list(old_next_size,old_next_bp);//移除旧的next块
        add_to_list(size,bp);//添加新的合并块
        return bp;
    }

}
//堆一致性检查器
int mm_check(void) {
    char *bp;
    int free_blocks_linear = 0;
    int free_blocks_list = 0;
    //扫描整个堆
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        //检查是否8字节对齐
        if ((size_t)bp % 8 != 0) {
            printf("Error: %p is not aligned\n", bp);
            return 0;
        }
        //检查块是否大于等于最小值16
        if (GET_SIZE(HDRP(bp)) < 16 && GET_SIZE(HDRP(bp)) != DSIZE) { 
            printf("Size Error: %p\n", bp); return 0; 
        }
        if (!GET_ALLOC(HDRP(bp))) {
            free_blocks_linear++; //线性扫描记录空闲块数量
            // 检查 Footer 是否存在且匹配
            if (GET(HDRP(bp)) != GET(FTRP(bp))) {
                printf("Footer Mismatch: %p\n", bp);
                return 0;
            }
        }
        //检查是否有连续空闲块
        if (!GET_ALLOC(HDRP(bp)) && !GET_ALLOC(HDRP(NEXT_BLKP(bp)))) {
            printf("Error: contiguous free blocks at %p\n", bp);
            return 0;
        }
        //检查pre_alloc记录是否正确
        if (GET_PRE_ALLOC(HDRP(NEXT_BLKP(bp))) != GET_ALLOC(HDRP(bp))) {
            printf("Error: prev_alloc bit mismatch at %p\n", NEXT_BLKP(bp));
            return 0;
        }
        //扫描分离链表
        for (int i = 0; i < LIST_COUNT; i++) {
            void *fbp = (void *)GET(free_list_start + i * WSIZE);
            while (fbp != NULL) {
                free_blocks_list++;
                // A. 指针合法性：空闲块地址必须在 mem_heap_lo 和 hi 之间
                if (fbp < mem_heap_lo() || fbp > mem_heap_hi()) {
                    printf("Error: Free list pointer %p is out of heap bounds\n", fbp);
                    return 0;
                }
                // B. 检查块是否真的空闲
                if (GET_ALLOC(HDRP(fbp))) {
                    printf("Error: Block %p in free list is marked as allocated\n", fbp);
                    return 0;
                }
                //检查当前块的next的prev是不是自己
                void *next_bp = (void *)(size_t)GET(NEXT_FREE(bp)); // 获取链表里的下一个块
                if (next_bp != NULL) {
                    if ((void *)GET(PREV_FREE(next_bp)) != (void *)bp) {
                        printf("Consistency Error: Block %p's next is %p, "
                                "but %p's prev is %p (should be %p)\n", 
                                (void *)bp, (void *)next_bp, (void *)next_bp, (void *)(size_t)GET(PREV_FREE(next_bp)), bp);
                        return 0;
                    }
                }        
            }
        }
        if (free_blocks_linear != free_blocks_list) {
        printf("Error: Linear scan found %d free blocks, but List scan found %d\n", 
                free_blocks_linear, free_blocks_list);
        return 0;
        }

    }
    return 1;
}










