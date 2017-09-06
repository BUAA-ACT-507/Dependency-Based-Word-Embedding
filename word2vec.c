//  Copyright 2013 Google Inc. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <pthread.h>

#define MAX_STRING 100//一个word的最大长度
#define EXP_TABLE_SIZE 1000//对f的运算结果进行缓存，存储1000个，需要使用时查表
#define MAX_EXP 6//最大计算到(exp^6 / (exp^6 + 1)),最小计算到(exp^-6 / (exp^-6 + 1))
#define MAX_SENTENCE_LENGTH 1000//句子的最大长度为1000
#define MAX_LINE 10000//一行的最大长度为10000
#define MAX_CODE_LENGTH 40//最长的霍夫曼编码长度
#define MAX_YICUN 6000//最多的依存关系个数
#define PI (atan(1.0) * 4)//宏定义PI

const int vocab_hash_size = 30000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary//哈希线性探测开放地址法，装填系数0.7

typedef float real;                    // Precision of float numbers

struct vocab_word {
	long long cn;//单词词频
	int *point;//霍夫曼树中从根节点到该词的路径，存放每个非叶子结点的索引
	char *word, *code, codelen;//词的字面表达，霍夫曼编码，编码长度
};

typedef struct node
{
	long long word;//hash
	real score;
	long long yicun[10];
	long long jie;
	struct node *next;
}Node, *sNode;

char train_file[MAX_STRING], output_file[MAX_STRING], new_output_file[MAX_STRING], weight_output_file[MAX_STRING];//训练文件名称，输出文件名称
char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING], read_weightcn[MAX_STRING];//词汇表输入文件名称，与词汇表输出文件名称
real weight[MAX_YICUN];//所有依存关系的权重
int weightcn[MAX_YICUN];//所有依存关系的次数
real multi[10] = {1, 1.2, 1.4, 1.8, 2.5, 3.4, 5, 6, 7, 8};//乘法系数
real premulti[10] = {1, 0.8, 0.6, 0.4, 0.3, 0.2, 0.1, 0.08, 0.06, 0.05};//乘法系数
struct vocab_word *vocab;//声明词汇表结构体
int binary = 0, cbow = 1, debug_mode = 2, window = 5, min_count = 5, num_threads = 12, min_reduce = 1;
//binary 0表示输出文件为二进制（默认），1表示输出文件为文本文件
//cbow 1表示使用cbow框架，0表示使用skip-gram框架
//debug_mode 大于0，加载完毕后输出汇总信息，大于1，加载训练词汇的时候输出信息，训练过程中输出信息
//window 窗口大小，表示cbow中word vector的范围，也表示skip-gram中max space between words
//min_count 表示删除长尾词的词频标准
//num_threads 表示线程数
//min_reduce 删除词频小于该值的词语，hash表装填词数是有限的

int new_operation = 0;
//新的依存关系的操作

int *vocab_hash;//词汇表的hash存储，下标是词的hash，内容是词的在vocab结构中的位置，a[word_hash]=word index in vocab
long long vocab_max_size = 1000, vocab_size = 0, layer1_size = 100, weight_layer_size = 50;
//vocab_max_size 词汇表的最大长度，可每次扩容1000
//vocab_size 词汇表的现有长度，接近最大值时会扩增
//layer1_size 隐层的节点数
long long train_words = 0, word_count_actual = 0, iter = 5, file_size = 0, classes = 0, train_weights = 0, weight_size = 0;
//train_words 训练的单词总数（词频累加）
//word_count_actual 以训练完的word的个数
//file_size 训练文件大小，ftell得到
//classes 输出word_clusters的类别数
//real alpha = 0.025, starting_alpha, sample = 1e-3;
real alpha = 0.015, starting_alpha, sample = 1e-3, weight_sample = 1e-10;
//alpha 表示学习率
//starting_alpha 表示初始学习率
//sample 亚采样概率，亚采样用来一定频率拒绝高频词，使得低频词由更多的出现几率
real *syn0, *syn1, *syn1neg, *expTable, *syn2;
//syn0 单词的向量输入 concatenate word vectors
//syn1 hs(hierarchical softmax)算法中隐层节点到霍夫曼编码树非叶子结点的映射权值
//syn2 额外的权重向量
//syn1neg ns(negative sampling)中隐层节点到分类的映射权重
//expTable 预先存储f函数结果，算法执行查表
clock_t start;

int hang = 0;
int hs = 0, negative = 5;
//hs 表示采用hs还是ns，默认hs
const int table_size = 1e8;//静态采样表的规模
int *table;//采样表

void InitUnigramTable() {//根据词频生成采样表
	int a, i;
	long long train_words_pow = 0;
	real d1, power = 0.75;//概率与词频的power次方成正比
	table = (int *)malloc(table_size * sizeof(int));//为采样表划分内存
	for (a = 0; a < vocab_size; a++) train_words_pow += pow(vocab[a].cn, power);//计算词汇表中单词词频^0.75的总数
	i = 0;
	d1 = pow(vocab[i].cn, power) / (real)train_words_pow;//第一个词出现的概率
	for (a = 0; a < table_size; a++) {
		table[a] = i;
		if (a / (real)table_size > d1) {//如果当遍历操作指针位置与采样表总数的比例大于目前采样样本词频总和则取样
			i++;
			d1 += pow(vocab[i].cn, power) / (real)train_words_pow;
		}
		if (i >= vocab_size) i = vocab_size - 1;//处理最后一段概率，防止越界
	}
}

// Reads a single word from a file, assuming space + tab + EOL to be word boundaries
void ReadWord(char *word, FILE *fin) {//每次从fin中获取一个单词
	int a = 0, ch;
	while (!feof(fin)) {
		ch = fgetc(fin);//从fin读取一个字符
		if (ch == 13) continue;//将ASCII编码为13的字符略过
		if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {//当读到' ','\t','\n'时
			if (a > 0) {//不是读到的第一个字符
				if (ch == '\n') ungetc(ch, fin);//退回一个字符到输入流中
				break;
			}
			if (ch == '\n') {//读到的第一个字符是'\n'，返回空字符
				strcpy(word, (char *)"</s>");
				return;
			}
			else continue;
		}
		word[a] = ch;//不是上述字符则保存
		a++;//位置后移一位
		if (a >= MAX_STRING - 1) a--;   // Truncate too long words
	}
	word[a] = 0;
}

int ReadNum(FILE* fin){
	int readnum = 0;
	int count = 0;
	char ch;
	while (!feof(fin)){
		ch = fgetc(fin);
		if (ch == 13) continue;
		if ((ch == ' ') || (ch == '\t') || (ch == '\n')){
			if (count > 0) {
				if (ch == '\n') {
				//ungetc(ch, fin);
				break;
				}
			}
			if (ch == '\n') { 
				return 0;
			}
			else continue;
		}
		if (isdigit(ch)){
			readnum = 10 * readnum + (ch - '0');
		}
		count++;
		if (count >= MAX_STRING - 1) count--;
	}
	return readnum;
}

// Returns hash value of a word
int GetWordHash(char *word) {//返回一个单词的hash值
	unsigned long long a, hash = 0;
	for (a = 0; a < strlen(word); a++) hash = hash * 257 + word[a];//hash计算方法
	hash = hash % vocab_hash_size;
	return hash;
}

// Returns position of a word in the vocabulary; if the word is not found, returns -1
int SearchVocab(char *word) {//寻找单词在词汇表位置
	unsigned int hash = GetWordHash(word);//计算单词hash值
	if (strcmp(word, "</s>") == 0) return -2;
	while (1) {
		if (vocab_hash[hash] == -1) return -1;//未找到返回-1
		if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];//匹配到则返回词汇表索引位置
		hash = (hash + 1) % vocab_hash_size;//hash开放地址法继续寻找
	}
	return -1;//无用
}

// Reads a word and returns its index in the vocabulary
int ReadWordIndex(FILE *fin) {//读入一个单词 并返回其词汇表索引
	char word[MAX_STRING];
	ReadWord(word, fin);
	if (feof(fin)) return -1;//如果读取到文件尾 返回-1
	//printf("%s\t",word);
	return SearchVocab(word);
}

int new_word(Node* tail, char *line, long long b, long long a) {
	long long i, c, num, jie, status = 0;
	real score = 1;
	char word[MAX_STRING];
	memset(word, 0, MAX_STRING);
	sNode temp = (sNode)malloc(sizeof(Node));
	i = c = num = jie = status = 0;
	for (c = 0; c < 10;c++) {
		temp->yicun[c] = -1;
	}
	c = status = num = jie = i = 0;
	for (i = b;i < a;i++) {
		if (line[i] == ' ' || line[i] == '\t') {
			continue;
		}
		if (status == 0) {
			if (isalpha(line[i])) {
				word[c] = line[i];
				c++;
				if (c >= MAX_STRING - 1) {
					c--;
				}
			}
			if (!isalpha(line[i+1]) || i == a - 1) {
				status = 1;
				word[c] = 0;
				//printf("%s\t", word);
				temp->word = SearchVocab(word);
				//printf("%s,%d***%s\t", vocab[temp->word].word,temp->word,word);
			}
		}
		else if (status == 1) {
			if (line[i] == ',') {
				//save
				temp->yicun[jie++] = num;
				//printf("%d\t", num);
				num = 0;
			}
			if(i < a){
				if (i + 1 == a - 1 && isdigit(line[i])){
					num = num * 10 + (line[i] - '0');
					temp->yicun[jie++] = num;
					//printf("%d\t", num);
					//save
					num = 0;
					i++;
					break;
				}
				else if(isdigit(line[i]) && (!isdigit(line[i+1])) && line[i+1] != ',' && (line[i + 1] == ' ')) {
					num = num * 10 + (line[i] - '0');
					temp->yicun[jie++] = num;
					//printf("%d\t", num);
					//save
					num = 0;
					i++;
					break;
				}
				else if (isdigit(line[i]) && i + 1 != a - 1 && line[i + 1] != ' '){
					num = num * 10 + (line[i] - '0');
					//printf("%dbbb  ", num);
				}
			}
		}
	}
	//printf("%d阶\t", jie);
	score = 1;
	for (c = 0;c < 10;c++) {
		if (temp->yicun[c] == -1) {
			continue;
		}
		else {
			num = temp->yicun[c];
			//printf("%f ", ((weight[num]) / multi[c]));
			//score = (score) * ((weight[num]) / multi[c]);
			score *= (weight[num]);
			num = c;
			//printf("%d\t", c);
			
			//printf("\n");
		}
	}
	for (status = 0;status <= num;status++){
				score /= multi[status];
				//printf("%f\t", score);
	}
	temp->score = score;
	temp->jie = jie;
	temp->next = NULL;
	tail->next = temp;
	tail = tail->next;
	return i;
	//return ++b;
}

// Reads a line and get all scores
void GetScore(FILE *fin, Node* head, Node* tail) {//读入一行，并求得所有的score（链表中的值），并保存每个单词所对应的所有依存关系（链表）
	long long a, b, c, status;
	char ch;
	char word[MAX_STRING];
	char line[MAX_LINE];
	memset(line, 0, MAX_LINE);
	if (feof(fin)) return;
	a = b = c = status = 0;
	while (!feof(fin)) {
		ch = fgetc(fin);
		if (ch == 13) continue;
		if (ch == '\n' || feof(fin)) {
			//printf("%s~~~~~~~~~~~~~~~~~\n",line);
			if (a == 0) {
				line[0] = '1';
				return;
			}
			line[a] = '\0';
			a++;
			//处理
			//status0-word, status1-yicun
			status = 0;
			//printf("%d\n",a);
			//printf("step 1\t");
			for (b = c = 0;b < a;b++) {
				//step1: get target word
				//step2: change to anther f(x), to get word, yicun
				//step3: every word & yicun, a f(x)
				if (status == 0 && isalpha(line[b])) {
					word[c] = line[b];
					c++;
					if (c >= MAX_STRING - 1) {
						c--;
					}
				}
				if (status == 0 && (!isalpha(line[b]))) {
					status = 1;
					word[c] = 0;
					//printf("%s\t", word);
					tail->word = SearchVocab(word);
				}
				if (status == 1) {
					b = new_word(tail, line, b, a);
					if (tail->next->word == -1){
						free(tail->next);
						tail->next = NULL;
					}
					else{
						tail = tail->next;
					}
					//printf("%f ", tail->score);
					//printf("%d %d\n",tail->score,tail->jie);
				}
			}
			if (a > 0) {
				if (ch == '\n') {
					return;
				}
			}
			return;
		}
		else {
			if(a < MAX_LINE - 1)
			{
				line[a] = ch;
				a++;
			}
			else{
				continue;
			}
		}
	}
	return;
}


// Adds a word to the vocabulary
int AddWordToVocab(char *word) {//在词汇表中添加一个单词
	unsigned int hash, length = strlen(word) + 1;
	if (length > MAX_STRING) length = MAX_STRING;
	vocab[vocab_size].word = (char *)calloc(length, sizeof(char));//为新添加单词新建内存空间并初始化0
	strcpy(vocab[vocab_size].word, word);
	vocab[vocab_size].cn = 0;//词频记0
	vocab_size++;//词汇表现存有单词数
				 // Reallocate memory if needed
	if (vocab_size + 2 >= vocab_max_size) {//如果词汇表将要存满
		vocab_max_size += 1000;//扩容1000
		vocab = (struct vocab_word *)realloc(vocab, vocab_max_size * sizeof(struct vocab_word));//动态扩容词汇表内存空间
	}
	hash = GetWordHash(word);//计算单词hash值
	while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;//如果hash对应位置不为空，则线性探索
	vocab_hash[hash] = vocab_size - 1;//记录单词在词汇表中的索引
	return vocab_size - 1;//返回单词在词汇表中的索引位置
}

// Used later for sorting by word counts
int VocabCompare(const void *a, const void *b) {//单词比较，使用单词词频进行词汇表排序
	return ((struct vocab_word *)b)->cn - ((struct vocab_word *)a)->cn;
}

// Sorts the vocabulary by frequency using word counts
void SortVocab() {//按照词频排序
	int a, size;
	unsigned int hash;
	// Sort the vocabulary and keep </s> at the first position
	qsort(&vocab[1], vocab_size - 1, sizeof(struct vocab_word), VocabCompare);//对词汇表进行快速排序
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;//词汇表顺序重建，hash表需要重建
	size = vocab_size;
	train_words = 0;
	for (a = 0; a < size; a++) {
		// Words occuring less than min_count times will be discarded from the vocab
		if ((vocab[a].cn < min_count) && (a != 0)) {//词频低于最小值则删除
			vocab_size--;
			free(vocab[a].word);
		}
		else {//重建hash表
			  // Hash will be re-computed, as after the sorting it is not actual
			hash = GetWordHash(vocab[a].word);
			while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;//不为空则线性探索
			vocab_hash[hash] = a;//记录单词在词汇表中的索引位置
			train_words += vocab[a].cn;//需要训练的词频累加
		}
	}
	vocab = (struct vocab_word *)realloc(vocab, (vocab_size + 1) * sizeof(struct vocab_word));//释放多余空间
																							  // Allocate memory for the binary tree construction
	for (a = 0; a < vocab_size; a++) {//为词汇表中单词分配霍夫曼编码与路径的存储空间
		vocab[a].code = (char *)calloc(MAX_CODE_LENGTH, sizeof(char));
		vocab[a].point = (int *)calloc(MAX_CODE_LENGTH, sizeof(int));
	}
}

// Reduces the vocabulary by removing infrequent tokens
void ReduceVocab() {//通过移除低频词汇减少词汇
	int a, b = 0;
	unsigned int hash;
	for (a = 0; a < vocab_size; a++) if (vocab[a].cn > min_reduce) {//循环记录大于移除阀值的单词
		vocab[b].cn = vocab[a].cn;
		vocab[b].word = vocab[a].word;
		b++;
	}
	else free(vocab[a].word);
	vocab_size = b;//新的词汇表单词个数
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;//hash清空
	for (a = 0; a < vocab_size; a++) {//重计算hash表
									  // Hash will be re-computed, as it is not actual
		hash = GetWordHash(vocab[a].word);
		while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
		vocab_hash[hash] = a;
	}
	fflush(stdout);
	min_reduce++;//移除阀值加1
}

// Create binary Huffman tree using the word counts
// Frequent words will have short uniqe binary codes
void CreateBinaryTree() {//根据词频生成霍夫曼树
	long long a, b, i, min1i, min2i, pos1, pos2, point[MAX_CODE_LENGTH];
	char code[MAX_CODE_LENGTH];
	long long *count = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));//记录原词汇表与点f合并后的点的词频
	long long *binary = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));//记录词汇表与点合并后的点对应位置的二进制编码
	long long *parent_node = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));//记录原词汇表中点合并后的对应位置
	for (a = 0; a < vocab_size; a++) count[a] = vocab[a].cn;//将词汇表单词词频统计在count中
	for (a = vocab_size; a < vocab_size * 2; a++) count[a] = 1e15;//在count后补齐一个vocab个数的1*10^15 记录后续词频和用
	pos1 = vocab_size - 1;
	pos2 = vocab_size;
	// Following algorithm constructs the Huffman tree by adding one node at a time
	//每次增加一个点
	for (a = 0; a < vocab_size - 1; a++) {
		// First, find two smallest nodes 'min1, min2'
		//寻找两个词频最小的的点合并，较小的为0，较大的为1
		if (pos1 >= 0) {//未到词汇表表首
			if (count[pos1] < count[pos2]) {//寻找词频最低的单词
				min1i = pos1;
				pos1--;
			}
			else {
				min1i = pos2;
				pos2++;
			}
		}
		else {
			min1i = pos2;
			pos2++;
		}
		if (pos1 >= 0) {
			if (count[pos1] < count[pos2]) {//寻找词频第二低的单词
				min2i = pos1;
				pos1--;
			}
			else {
				min2i = pos2;
				pos2++;
			}
		}
		else {
			min2i = pos2;
			pos2++;
		}
		count[vocab_size + a] = count[min1i] + count[min2i];//原词汇表后接的第a个位置写入两个被合并的节点的词频和
		parent_node[min1i] = vocab_size + a;//记录该单词合并后的节点在词汇表后续中的位置
		parent_node[min2i] = vocab_size + a;
		binary[min2i] = 1;//二进制记录数组中相应位置被标记
	}
	// Now assign binary code to each vocabulary word
	//沿树的父子关系构成编码
	for (a = 0; a < vocab_size; a++) {
		b = a;
		i = 0;
		while (1) {
			code[i] = binary[b];//编码赋值
			point[i] = b;//路径赋值，第一个节点为自己
			i++;//编码个数
			b = parent_node[b];
			if (b == vocab_size * 2 - 2) break;
		}
		//以下point比code多记录一层
		vocab[a].codelen = i;//编码总长度，较实际少1，未计算根节点
		vocab[a].point[0] = vocab_size - 2;//逆序，将第一个赋值为root
		for (b = 0; b < i; b++) {//为vocab[a]装载相应霍夫曼编码（逆序）
			vocab[a].code[i - b - 1] = code[b];//逆序编码，左子树为1，右子树为0
			vocab[a].point[i - b] = point[b] - vocab_size;//逆序赋值，记录路径节点
		}
	}
	free(count);
	free(binary);
	free(parent_node);
}

void LearnVocabFromTrainFile() {//装载训练文件到词汇表
	char word[MAX_STRING];
	FILE *fin;
	long long a, i;
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;//初始化
	fin = fopen(train_file, "rb");
	if (fin == NULL) {//文件不存在
		printf("ERROR: training data file not found!\n");
		exit(1);
	}
	vocab_size = 0;//词汇表词汇数量置零
	AddWordToVocab((char *)"</s>");//首先添加回车
	while (1) {
		ReadWord(word, fin);//读入一个正确单词
		if (feof(fin)) break;//文件结尾则中断循环
		train_words++;//读入词汇数量+1
		if ((debug_mode > 1) && (train_words % 100000 == 0)) {//根据输出模式打印训练信息
			printf("%lldK%c", train_words / 1000, 13);
			fflush(stdout);
		}
		i = SearchVocab(word);//寻找其在词汇表的索引位置
		if (i == -1) {//不存在则添加 词频置一
			a = AddWordToVocab(word);
			vocab[a].cn = 1;
		}
		else vocab[i].cn++;//存在词频+1
		if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();//如果词汇表装入词汇达到阈值 则扩容
	}
	SortVocab();//所有词汇添加完毕 进行排序
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);
	}
	file_size = ftell(fin);//记录文件大小
	fclose(fin);
}

void SaveVocab() {//存储词汇表内容到文件
	long long i;
	FILE *fo = fopen(save_vocab_file, "wb");
	for (i = 0; i < vocab_size; i++) fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn);
	fclose(fo);
}

void ReadVocab() {//从文件直接读入词汇表结构
	long long a, i = 0;
	char c;
	char word[MAX_STRING];
	FILE *fin = fopen(read_vocab_file, "rb");
	if (fin == NULL) {
		printf("Vocabulary file not found\n");
		exit(1);
	}
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	vocab_size = 0;
	while (1) {//读入单词及词频 并添加到词汇表中
		ReadWord(word, fin);
		if (feof(fin)) break;
		a = AddWordToVocab(word);
		fscanf(fin, "%lld%c", &vocab[a].cn, &c);
		i++;
	}
	SortVocab();//排序
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);
	}
	fin = fopen(train_file, "rb");//打开训练文件 记录文件大小
	if (fin == NULL) {
		printf("ERROR: training data file not found!\n");
		exit(1);
	}
	fseek(fin, 0, SEEK_END);
	file_size = ftell(fin);
	fclose(fin);
}
//网络结构初始化

void ReadWeightcn() {
	int num = 0;
	int count = 0;
	FILE *fin = fopen(read_weightcn, "rb");
	//printf("\nstart\n");
	if (fin == NULL) {
		printf("Weightcn file not found\n");
		exit(1);
	}
	while (1) {
		//if (count == 10) exit(1);
		num = 0;
		num = ReadNum(fin);
		if (feof(fin)) break;
		//printf("%d\n",num);
		//printf("~~~~~~~~%d\n",count);
		weightcn[count] = num;
		train_weights += num;
		count++;
	}
	weight_size = count;
	printf("Weights in train file: %d\n", count);
	fclose(fin);
}


void InitNet() {
	long long a, b;
	unsigned long long next_random = 1;
	a = posix_memalign((void **)&syn2, 128, (long long)weight_size * weight_layer_size * sizeof(real));
	a = posix_memalign((void **)&syn0, 128, (long long)vocab_size * layer1_size * sizeof(real));
	//posix_memalign成功时会返回size字节的动态内存，并且这块内存的地址是aligment的倍数
	//syn0为word vectors
	if (syn0 == NULL) { printf("Memory allocation failed\n"); exit(1); }
	if (syn2 == NULL) { printf("Memory allocation failed\n"); exit(1); }
	if (hs) {//在hisrarchical softmax中
		a = posix_memalign((void **)&syn1, 128, (long long)vocab_size * (layer1_size + weight_layer_size) * sizeof(real));
		if (syn1 == NULL) { printf("Memory allocation failed\n"); exit(1); }
		for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++)
			syn1[a * (layer1_size + weight_layer_size) + b] = 0;//初始化
	}
	if (negative>0) {//negative sampling
		a = posix_memalign((void **)&syn1neg, 128, (long long)vocab_size * (layer1_size + weight_layer_size) * sizeof(real));
		if (syn1neg == NULL) { printf("Memory allocation failed\n"); exit(1); }
		for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++)
			syn1neg[a * (layer1_size + weight_layer_size) + b] = 0;
	}
	for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++) {//初始化随机为word vectors赋值
		next_random = next_random * (unsigned long long)25214903917 + 11;
		syn0[a * layer1_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5) / layer1_size;//syn0将词汇表中单词按照顺序依次将该单词对应word vector存储在syn0上，syn[0]-syn[layer1_size - 1]表示一个单词
	}
	next_random = 1;
	for (a = 0; a < weight_size; a++) for (b = 0; b < weight_layer_size; b++) {//初始化随机为weight vectors赋值
		next_random = next_random * (unsigned long long)25214903917 + 11;
		syn2[a * weight_layer_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5) / weight_layer_size;
	}
	CreateBinaryTree();
}
//插入排序排score顺序
Node* Insert_sort(Node* head) {
	Node* first;
	Node* t;
	Node* p;
	Node* q;

	if (head == NULL || head->next == NULL) return head;
	first = head->next;
	head->next = NULL;
	while (first != NULL) {
		for (t = first, q = head; ((q!= NULL) && (q->score >= t->score)); p = q, q = q->next);

		first = first->next;
		if (q == head) /*插在第一个节点之前*/
		{
			head = t;
		}
		else /*p是q的前驱*/
		{
			p->next = t;
		}
		t->next = q;
	}

	return head;
}

void *TrainModelThread(void *id) {
	long long a, b, d, z, cw, word, last_word, sentence_length = 0, sentence_position = 0;
	//word表示句子中当前选定的单词
	//last_word表示句子上一个选定的单词
	//sentence_length表示句子中的单词数
	//sentence_position表示句子中的当前选定位置
	long long learn_word_count = 0, last_learn_word_count = 0, word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1];
	//word_count表示已训练的单词数
	//last_word_count存储值
	//sen[]表示句子数组
	long long l1, l2, c, target, label, local_iter = iter;
	int randomjie, randomyicun[5];
	//l1表示ns中word在concatenated word vectors中的起始位置，之后layer1_size对应word vector,把矩阵拉长成为向量
	//l2表示cbow或ns中权重向量的起始位置，之后layer1_size对应syn1或syn1neg,把矩阵拉长成为向量
	//c在循环中起计数作用
	//target表示ns中的sample
	//label表示ns中当前sample的label
	unsigned long long next_random = (long long)id;
	unsigned long long next_random_s = (long long)id;
	//id线程创建时传入，随机生成
	real f, g;
	//f 表示e^x / (1 + e^x), hs中指当前编码为0的概率，ns中指label为1的概率
	//g 表示误差（f与实际情况的差距）与学习效率的乘积
	real sum = 0;
	//sum用于CBOW模型的输入层到投影层
	real average = 0;
	int count = 0;
	int wcount = 0;
	int sizew = 0;
	long long number = 0;
	cw = 0;
	clock_t now;
	real lamda = 0;
	real multir = 0;
	real moda = 0;
	real modb = 0;
	real multiresult = 0;
	sNode head = NULL;
	sNode tail = NULL;
	//当前时间
	real *neu1 = (real *)calloc(layer1_size + weight_layer_size, sizeof(real));//创建隐层节点存储空间
	real *neu1e = (real *)calloc(layer1_size + weight_layer_size, sizeof(real));//误差累计项，对应的Gneu1
	real *neu1w = (real *)calloc(layer1_size + weight_layer_size, sizeof(real));//更新weoght时所用误差
	FILE *fi = fopen(train_file, "rb");
	FILE *new_operation_fi = fopen(train_file, "rb");
	fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);//将文件内容分配给各线程
	fseek(new_operation_fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);//将文件内容分配给各线程
	int zd = 0;
	while (1) {
		zd++;
		head = (sNode)malloc(sizeof(Node));
		tail = head;
		tail->next = NULL;
		if (learn_word_count - last_learn_word_count > 10000) {//学习10000词后输出并更新学习率
			word_count_actual += learn_word_count - last_learn_word_count;
			last_learn_word_count = learn_word_count;
			if ((debug_mode > 1)) {//输出学习进度
				now = clock();
				printf("%cAlpha: %f  Progress: %.2f%%  Words/thread/sec: %.2fk  ", 13, alpha,
					word_count_actual / (real)(iter * train_words + 1) * 100,
					word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
				fflush(stdout);
			}
			alpha = starting_alpha * (1 - word_count_actual / (real)(iter * train_words + 1));//自动调整学习率
			if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;//设置学习率下限
		}
		if (sentence_length == 0) {//如果当前句子长度为0
			//printf("%d\n",number++);
			if (new_operation == 1 && !feof(new_operation_fi)) {
				GetScore(new_operation_fi, head, tail);
				head->score = 0;
				/*printf("**************\n");
				Node *p = head;
				while(p!=NULL)
				{
					printf("%s\t%f\t",vocab[p->word].word,p->score);
					p=p->next;
				}*/
				//printf("**************\n");
				head->next = Insert_sort(head->next);
				/*p = head;
				while(p!=NULL)
				{
					printf("%s\t%f\t",vocab[p->word].word,p->score);
					p=p->next;
				}
				printf("\n\n\n");*/
				//if(zd==3)break;
				//for(;p!=NULL;)
				//{
				//	printf("%d阶\t", p->jie);
				//	p = p->next;
				//}
				//printf("\n**********\n");
			}
			while (1) {
				word = ReadWordIndex(fi);//读入单词索引
				if (feof(fi)) break;//文件结尾则中断循环
				if (word == -2) break;
				if (word == -1) continue;//没有单词 继续循环
				word_count++;//不属于上述情况 单词数+1
				if (word == 0) break;//读到回车中断循环
									 // The subsampling randomly discards frequent words while keeping the ranking same
				/*if (sample > 0) {//亚采样
					real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
					next_random = next_random * (unsigned long long)25214903917 + 11;
					if (ran < (next_random & 0xFFFF) / (real)65536) continue;
				}*/
				sen[sentence_length] = word;//在句子中存储读入的单词
				sentence_length++;//句子长度+1
				if (sentence_length >= MAX_SENTENCE_LENGTH) break;//句子长度不超过设置阈值
			}
	        learn_word_count++;
			sentence_position = 0;//读完一个句子 单词在句子中位置置0
		}
		if (feof(fi) || (learn_word_count > train_words / num_threads)) {//读到文件结尾或者完成分配工作
			word_count_actual += learn_word_count - last_learn_word_count;;
			local_iter--;
			if (local_iter == 0) break;
			word_count = 0;
            last_word_count = 0;
	        last_learn_word_count = 0;
	        learn_word_count = 0;
			sentence_length = 0;
			fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
			fseek(new_operation_fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
			continue;
		}
		word = sen[0];//取句子中第一个单词 开始BP算法
		if (word == -1) continue;//如果单词不存在 则继续循环
		for (c = 0; c < layer1_size + weight_layer_size; c++) neu1[c] = 0;//隐层节点值清零
		for (c = 0; c < layer1_size + weight_layer_size; c++) neu1e[c] = 0;//隐层节点误差值累计清零
		for (c = 0; c < layer1_size + weight_layer_size; c++) neu1w[c] = 0;//隐层节点误差值（权重）累计清零
		next_random_s = next_random_s* (unsigned long long)25214903917 + 11;
		b = next_random_s % window;//随机生成本次操作的窗口大小 (window - b)表示窗口大小
		//b = 3;
		if (cbow) {  //train the cbow architecture
					 // in -> hidden
					 //将输入层的窗口内输入累加到隐层节点上

					 //现在不是普通的累加。而是取合适的window词累加
			sum = 0;
			count = 0;
			if (new_operation == 1) {
				//b = 11 - log10(vocab[sen[0]].cn);
				//if (4 > b) b = 4;
				//sizew = b;
				/*Node *n;
				n = head->next;
				a = 1;
				while (a < (2 * (window - b)) && n!=NULL && n->score > 0.6){		
					count++;
					sum += n->score;
					//n->score = 1;
					a++;
					for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + n->word * layer1_size];
					n = n->next;
				}
				if (count == 0) count = 1;
				average = sum / count;
				average = 1;*/
				//printf("suiji:%d\n",b);
				Node *n,*p;
				/*p = head;
				while(p!=NULL)
				{
					printf("%s\t",vocab[p->word].word);
					p=p->next;
				}
				printf("\n");*/
				
				Node *h;
				h = head;
				//next_random_s = next_random_s * (unsigned long long)25214903917 + 11;
				for(;h->next!=NULL;)
				{
					if (sample > 0) {//亚采样
						real ran = (sqrt(vocab[h->next->word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[h->next->word].cn;
						next_random_s = next_random_s * (unsigned long long)25214903917 + 11;
						//printf("%I64u ",next_random);
						if (ran < (next_random_s & 0xFFFF) / (real)65536) {
								//printf("qudiao:%s\t",vocab[h->next->word].word);
								p = h->next;
								h->next = h->next->next;
								free(p);
						}
						else{
							//printf("liuxia:%s\t",vocab[h->next->word].word);
							h = h->next;
						}
					}
				}
				h = head;
				/*c = 0;
				for(;h!=NULL;){
					last_word = h->word;
					printf("%s\t%f\t",vocab[last_word].word,h->score);
					for (a = 0;h->yicun[a] != -1;a++){
						printf("%d,",h->yicun[a]);
					}
					printf("\t");
					c++;
					h = h->next;
				}
				printf("\nsentencelength:%d\n",c);*/
				n = head->next;
				
				/*printf("\n");
				//n = head->next;
				p = head;
				while(p!=NULL)
				{
					if(p->score > 0.6 || p->score == 0)
						printf("%s\t",vocab[p->word].word);
					p=p->next;
				}
				printf("\n**********************\n");*/
				//printf("%s\t",vocab[head->word].word
				average = 0;
				sum = 0;
				count = 0;
				//for (a = 1; a < window * 2 - b * 2 && n!=NULL && n->score > 0.6; a++,n=n->next) if (a != 0) {
				for (a = 1; a < window * 2 - b * 2 && n!=NULL ; a++,n=n->next) if (a != 0) {
				//b = 11 - log10(vocab[head->word].cn);
				//if (b < 4) b = 4;
				//printf("%d\t",b);
				//for (a = 1; a <= b && n!=NULL && n->score > 0.6; a++,n=n->next){
					c = a;
					if(c < 0) continue;
					//if (c >= sentence_length) continue;
					last_word = n->word;//单词在词汇表中索引位置
					if (last_word == -1) {
						printf("situation2\t");
						continue;
					}
					//for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + last_word * layer1_size] * n -> score;//累加单词word vector的各位值到隐层上
					for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + last_word * layer1_size] * n -> score;//累加单词word vector的各位值到隐层上
					for (c = 0;c < 10;c++) {
						if (n->yicun[c] == -1) {
							break;
						}
						else {
							for (d = layer1_size; d < weight_layer_size + layer1_size; d++){
								neu1[d] += syn2[(d - layer1_size) + (n->yicun[c] * weight_layer_size)] * premulti[c];
							}
							wcount++;
						}
					}
					if (c != 0) {wcount = c;} else{wcount = 1;}	
					for (c = layer1_size; c < weight_layer_size + layer1_size; c++) neu1[c] /= wcount;
					//for (c = layer1_size; c < weight_layer_size + layer1_size; c++) neu1[c] += syn2[(c - layer1_size) + last_word * weight_layer_size] * n -> score;//累加单词word vector的各位值到隐层上
					//修改，点乘权重，除以权重和（加权平均）
					//printf("%s\t",vocab[last_word].word);
					count++;
					sum += n -> score;
					
					//printf("%s\t",vocab[last_word].word);
				}
				if (count != 0){
					average = sum / count;
				}
				if (average == 0){
					average = 1;
				}
				//printf("\n%d---%d---%f---%f\n",window * 2 - b * 2,count,sum,average);
				//if(zd == 1000)break;
			}
			/*else {// layer1_size与依存？
				for (a = b; a < window * 2 + 1 - b; a++) if (a != 0) {
					c = a;
					if (c < 0) continue;
					if (c >= sentence_length) continue;
					last_word = sen[c];//单词在词汇表中索引位置
					if (last_word == -1) continue;
					if (new_operation == 1){
						for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + last_word * layer1_size] * (sum) / b;//累加单词word vector的各位值到隐层上
					}else{
						for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + last_word * layer1_size];
					}
					cw++;
				}
			}*/
			if (count) {
				for (c = 0; c < layer1_size; c++) neu1[c] /= count;//隐层word vector累加和取平均
				if (hs) for (d = 0; d < vocab[word].codelen; d++) {//code少根节点
					f = 0;
					l2 = vocab[word].point[d] * layer1_size;//路径上的点
					// Propagate hidden -> output
					for (c = 0; c < layer1_size + weight_layer_size; c++) f += neu1[c] * syn1[c + l2];//计算f值
					if (f <= -MAX_EXP) continue;
					else if (f >= MAX_EXP) continue;
					else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];//在exptable中寻找激活函数值 0~1
					// 'g' is the gradient multiplied by the learning rate
					g = (1 - vocab[word].code[d] - f) * alpha;//公式精简后的g
					// Propagate errors output -> hidden
					for (c = 0; c < layer1_size + weight_layer_size; c++) neu1e[c] += g * syn1[c + l2] * average;//隐层每个神经元记录积累误差值
					for (c = 0; c < layer1_size + weight_layer_size; c++) neu1w[c] += g * syn1[c + l2];//隐层每个神经元记录积累误差值（权重）
					// Learn weights hidden -> output
					for (c = 0; c < layer1_size + weight_layer_size; c++) syn1[c + l2] += g * neu1[c];//更新隐层到霍夫曼树非叶子结点的权重
				}
				// NEGATIVE SAMPLING
				if (negative > 0) for (d = 0; d < negative + 1; d++) {//负采样
					if (d == 0) {//设置正样本
						target = word;
						label = 1;
					}
					else {//随机选择与正样本不同的负样本 个数为negative个
						next_random_s = next_random_s * (unsigned long long)25214903917 + 11;
						target = table[(next_random_s >> 16) % table_size];
						if (target == 0) target = next_random_s % (vocab_size - 1) + 1;
						if (target == word) continue;
						label = 0;
					}
					l2 = target * layer1_size;//
					f = 0;
					for (c = 0; c < layer1_size; c++) f += neu1[c] * syn1neg[c + l2];//计算f值
					if (f > MAX_EXP) g = (label - 1) * alpha;//根据f值计算g值
					else if (f < -MAX_EXP) g = (label - 0) * alpha;
					else g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;
					//if (new_operation == 1){
					//	for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2] * average;//隐层每个神经元记录积累误差值1
					//}else{
						for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2] * average;//隐层每个神经元记录积累误差值
					//}
					for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * neu1[c];//更新隐层到霍夫曼树非叶子结点的权重																 
				}
				//保存阶数，用新的g更新依存权重
				// hidden -> in
				//for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
				//	c = sentence_position - window + a;
				//	if (c < 0) continue;
				//	if (c >= sentence_length) continue;
				//	last_word = sen[c];
				//	if (last_word == -1) continue;
				//	for (c = 0; c < layer1_size; c++) syn0[c + last_word * layer1_size] += neu1e[c];//根据隐层节点积累的误差 更新word vector
				//}
				//b = count;
				Node *nn;
				nn = head->next;
				
				
				/*lamda = 1;
				multir = 0;
				moda = 0;
				modb = 0;*/
				if (new_operation == 1){
					last_word = sen[0];//now_word, vector is syn0[c + last_word * layer1_size]
					Node *n;
					n = head->next;
					sizew = count;
					while(sizew > 0 && n!= NULL){
						lamda = 1;
						multir = 0;
						multiresult = 0;
						moda = 0;
						modb = 0;
						for (c = 0; c < layer1_size; c++) {
							multir += (neu1w[c] * syn0[c + n->word * layer1_size]);
							moda += (neu1w[c] * neu1w[c]); 
							modb += (syn0[c + last_word * layer1_size] * syn0[c + last_word * layer1_size]);
						}
						for (c = 0;c < 10;c++) {
							if (n->yicun[c] == -1) {
								break;
							}
							else {
								for (d = layer1_size; d < weight_layer_size + layer1_size; d++){
									multir += (neu1w[c] * syn2[(d - layer1_size) + (n->yicun[c] * weight_layer_size)]);
									//neu1[d] += syn2[(d - layer1_size) + (n->yicun[c] * weight_layer_size)] * premulti[c];
								}
							}
						}
						
						
						//for(c = layer1_size;c < layer1_size + weight_layer_size;c++){
						//	multir += (neu1w[c] * syn2[(c - layer1_size) + n->word * weight_layer_size]);
						//}
						for(a = 0;a < 10;a++){
							if (n->yicun[a] == -1) break;
							lamda *= multi[a];
							//for (c = 0; c < layer1_size; c++) {multir += (neu1e[c] * syn0[c + last_word * layer1_size]); moda += (neu1e[c] * neu1e[c]); modb = (syn0[c + last_word * layer1_size] * syn0[c + last_word * layer1_size]);}
							//printf("%f %f\n",moda,modb);
							//multir /= (moda * modb + 1);
							multiresult = multir/count;
							
							//multir *= g;//g?每个词对应的g？
							//weight[n->yicun[a]] += (atan(multiresult / lamda)) / PI; 
							if (weight_sample > 0) {//亚采样
								real ran = (sqrt(weightcn[n->yicun[a]] / (weight_sample * train_weights)) + 1) * (weight_sample * train_weights) / weightcn[n->yicun[a]];
								next_random = next_random * (unsigned long long)25214903917 + 11;
								if (ran < (next_random & 0xFFFF) / (real)65536){ 
									continue;
								}else{
									weight[n->yicun[a]] += (multiresult / lamda); 
									//if (word_count_actual / (real)(iter * train_words + 1) * 100 > 10){
									//	printf("%f--%d--%d--%s\n",weight[n->yicun[a]],n->yicun[a],sizew,vocab[n->word].word);
									//}
								}
							}
							//if(weight[n->yicun[a]] < 0.1) weight[n->yicun[a]] = 0.1;
							//if(weight[n->yicun[a]]<0.7) {printf("%f %d %f\n",weight[n->yicun[a]],n->yicun[a],multiresult);}
							//if(weight[n->yicun[a]]<0.1) getchar();
							//if(multir >= 0.001){
							//if (hang > 1000000){
							//	printf("%f--%d--%d--%s\n",weight[n->yicun[a]],n->yicun[a],sizew,vocab[n->word].word);
							//}
							//printf("**************************************\n");
							//}
							//printf("%f\t", (g * syn0[c + last_word * layer1_size]));
						}
						//printf("**************************************\n");
						sizew--;
						n = n->next;
					}
				}
				
				//n = head->next;
				//b = count;
				/*while (b > 0 && n!=NULL){
					last_word = n->word;
					for (c = 0; c < layer1_size; c++) {
						syn0[c + last_word * layer1_size] += neu1e[c];
					    //printf("%f\t%s\n",syn0[c + last_word * layer1_size],vocab[last_word].word);
					}
					b--;
					n = n->next;
				}*/
				/*a = 1;
				while (a < (2 * (window - b)) && nn!=NULL && nn->score > 0.6){
					last_word = nn->word;
					for (c = 0; c < layer1_size; c++) {
						syn0[c + last_word * layer1_size] += neu1e[c];
					    //printf("%f\t%s\n",syn0[c + last_word * layer1_size],vocab[last_word].word);
					}
					a++;
					nn = nn->next;
				}*/
				//for (a = 1; a < window * 2 - b * 2 && nn!=NULL && nn->score > 0.6; a++,nn=nn->next) if (a != 0) {
				//for (a = 1; a <= b && nn!=NULL && nn->score > 0.6; a++,nn=nn->next){
				for (a = 1; a <= b && nn!=NULL; a++,nn=nn->next){
					c = a;
					if (c < 0) continue;
					if (c >= sentence_length) continue;
					last_word = nn->word;
					if (last_word == -1) continue;
					for (c = 0; c < layer1_size; c++) syn0[c + last_word * layer1_size] += neu1e[c];//根据隐层节点积累的误差 更新word vector
					for (c = 0;c < 10;c++) {
						if (nn->yicun[c] == -1) {
							break;
						}
						else {
							for (d = layer1_size; d < weight_layer_size + layer1_size; d++){
								if (weight_sample > 0) {//亚采样
								real ran = (sqrt(weightcn[nn->yicun[c]] / (weight_sample * train_weights)) + 1) * (weight_sample * train_weights) / weightcn[nn->yicun[c]];
								next_random = next_random * (unsigned long long)25214903917 + 11;
								if (ran < (next_random & 0xFFFF) / (real)65536){ 
										continue;
									}else{
										syn2[(d - layer1_size) + nn->yicun[c] * weight_layer_size] += neu1e[d];
									}
								}
							}
						}
					}
					
					//for (c = layer1_size; c < layer1_size + weight_layer_size; c++) syn2[(c - layer1_size) + last_word * weight_layer_size] += neu1e[c];//根据隐层节点积累的误差 更新word vector
				}
			}
			hang++;
			//if (hang == 10){
			//	break;
			//}
			//break;
		}
		else {  //train skip-gram
				//使用skip-gram算法 一->多
			/*if (new_operation == 1) {
				count = 0;
				last_word = sen[0];
				b = 10 - log10(vocab[last_word].cn);
				if (4 > b) {
					b = 4;
				}
				sizew = b;
				//b == sizew
				//calculate score
				//依存关系共 5784 个
				Node *n;
				n = head->next;
				while (b > 0 && n!=NULL){
					count++;
					sum += n->score;
					b--;
					last_word = n->word;
					for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + last_word * layer1_size] * n->score;
					n = n->next;
				}
				if (count == 0) count = 1;
				average = sum / count;
			}*/
			if (new_operation == 1) {
				Node *n,*p;
				Node *h;
				h = head;
				next_random_s = next_random_s * (unsigned long long)25214903917 + 11;
				for(;h->next!=NULL;)
				{
					if (sample > 0) {//亚采样
						real ran = (sqrt(vocab[h->next->word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[h->next->word].cn;
						next_random_s = next_random_s * (unsigned long long)25214903917 + 11;
						if (ran < (next_random_s & 0xFFFF) / (real)65536) {
								p = h->next;
								h->next = h->next->next;
								free(p);
						}
						else{
							h = h->next;
						}
					}
				}
			n = head->next;
			
			for (a = 1; a < window * 2 - b * 2 && n!=NULL && n->score > 0.6; a++,n=n->next) if (a != 0) {
				c = a;
				if(c < 0) continue;
				if (c >= sentence_length) continue;
				last_word = n->word;//单词在词汇表中索引位置
				if (last_word == -1) continue;
				//l1 = last_word * layer1_size;
				l1 = last_word * layer1_size;
				for (c = 0; c < layer1_size + weight_layer_size; c++) neu1e[c] = 0;
				// HIERARCHICAL SOFTMAX
				if (hs) for (d = 0; d < vocab[word].codelen; d++) {
					f = 0;
					l2 = vocab[word].point[d] * layer1_size;
					// Propagate hidden -> output
					for (c = 0; c < layer1_size; c++) f += syn0[c + l1] * syn1[c + l2];
					if (f <= -MAX_EXP) continue;
					else if (f >= MAX_EXP) continue;
					else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
					// 'g' is the gradient multiplied by the learning rate
					g = (1 - vocab[word].code[d] - f) * alpha;
					// Propagate errors output -> 
					for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2];
					//for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2] * last_word->score;
					// Learn weights hidden -> output
					for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * syn0[c + l1];
				}
				// NEGATIVE SAMPLING
				//printf("\nns\n");
				if (negative > 0) for (d = 0; d < negative + 1; d++) {
					if (d == 0) {
						target = word;
						label = 1;
					}
					else {
						next_random_s = next_random_s * (unsigned long long)25214903917 + 11;
						target = table[(next_random_s >> 16) % table_size];
						if (target == 0) target = next_random_s % (vocab_size - 1) + 1;
						if (target == word) continue;
						label = 0;
					}
					randomjie = rand()%(4 - 1 + 1) + 1;
					for (c = 0;c < randomjie;c++){
						while (1){
							randomyicun[c] = rand()%(5999 - 0 + 1) + 0;
							if (weightcn[randomyicun[c]] == 0){
								continue;
							}else{
								break;
							}
						}
					}
					l2 = target * (layer1_size);
					//l2 = target * (layer1_size + weight_layer_size);
					f = 0;
					for (c = 0; c < layer1_size; c++) f += syn0[c + l1] * syn1neg[c + l2];
					for (c = 0;c < randomjie;c++){
						for (z = layer1_size; z < layer1_size + weight_size; z++) f += syn2[z - layer1_size + randomyicun[c] * weight_layer_size] * syn1neg[z + l2] * premulti[c];
					}
					//printf("\n%f\n",f);
					//for (c = layer1_size; c < layer1_size + weight_layer_size; c++) f += syn2[c - layer1_size + l1] * syn1neg[c + l2];
					if (f > MAX_EXP) g = (label - 1) * alpha;
					else if (f < -MAX_EXP) g = (label - 0) * alpha;
					else g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;
					for (c = 0; c < layer1_size + weight_layer_size; c++) neu1e[c] += g * syn1neg[c + l2];
					//for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2] * last_word->score;
					for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * syn0[c + l1];
					
					for (c = 0;c < randomjie;c++){
						for (z = layer1_size; z < layer1_size + weight_size; z++){
							syn1neg[z + l2] += g * syn2[z - layer1_size + (randomyicun[c] * weight_layer_size)];
						}
					}
					
					//(neu1w[c] * syn2[(d - layer1_size) + (n->yicun[c] * weight_layer_size)]);
					//for (c = layer1_size; c < layer1_size + weight_layer_size; c++) syn1neg[c + l2] += g * syn2[c - layer1_size + l1];
				}
				// Learn weights input -> hidden
				for (c = 0; c < layer1_size; c++) syn0[c + l1] += neu1e[c];
				for (c = 0;c < 10;c++) {
						if (n->yicun[c] == -1) {
							break;
						}
						else {
							for (z = layer1_size; z < weight_layer_size + layer1_size; z++){
								if (weight_sample > 0) {//亚采样
								real ran = (sqrt(weightcn[n->yicun[c]] / (weight_sample * train_weights)) + 1) * (weight_sample * train_weights) / weightcn[n->yicun[c]];
								next_random = next_random * (unsigned long long)25214903917 + 11;
								if (ran < (next_random & 0xFFFF) / (real)65536){ 
										continue;
									}else{
										syn2[(z - layer1_size) + n->yicun[c] * weight_layer_size] += neu1e[z];
									}
								}
							}
						}
				}
					
				//for (c = layer1_size; c < layer1_size + weight_layer_size; c++) syn2[c - weight_layer_size + l1] += neu1e[c];
				//update yicun weight
				//?
				/*if (new_operation == 1){
					last_word = sen[0];//now_word, vector is syn0[c + last_word * layer1_size]
					Node *n;
					n = head->next;
					b = sizew;
					while(b > 0 && n!= NULL){
						for(a = 0;a < 10;a++){
							if (n->yicun[a] == -1) continue;
							weight[n->yicun[a]] += (g * syn0[c + last_word * layer1_size]);
							for (c = 0;c < a;c++){
								weight[n->yicun[a]] = weight[n->yicun[a]] / multi[c];
							}
							//printf("%f\t", weight[n->yicun[a]]);
						}
						b--;
					}
					//printf("\n");
				}*/
			}
		}
		//sentence_position++;
		//if (sentence_position >= sentence_length) {
		//	hang++;
		//	if (hang == 1000){
		//		break;
		//	}
		}
			
		Node *p = head;
		Node *q = NULL;
		for(;p!=NULL;)
		{
			q = p;
			p = p->next;
			free(q);
		}
		//free(head);
		sentence_length = 0;
		continue;
		//}
	}
	fclose(fi);
	fclose(new_operation_fi);
	free(neu1);
	free(neu1e);
	free(neu1w);
	pthread_exit(NULL);
}

void TrainModel() {
	long a, b, c, d;
	FILE *fo;
	FILE *new_fo;
	FILE *weight_fo;
	pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));//创建多线程
	printf("Starting training using file %s\n", train_file);
	starting_alpha = alpha;
	if (read_vocab_file[0] != 0) ReadVocab(); else LearnVocabFromTrainFile();//优先从词汇表文件中读取 没有则到训练文件中加载
	if (save_vocab_file[0] != 0) SaveVocab();//如果词汇表不存在 保存词汇表
	if (read_weightcn[0] != 0) ReadWeightcn();
	//printf("%d\n",train_weights);
	if (output_file[0] == 0) return;
	if (new_output_file[0] == 0) return;
	InitNet();//初始化网络结构
	if (negative > 0) InitUnigramTable();//根据词频生成采样映射
	start = clock();//开始计时点
	for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
	for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
	fo = fopen(output_file, "wb");//训练结束 准备输出
	new_fo = fopen(new_output_file, "wb");//训练结束 准备输出
	weight_fo = fopen(weight_output_file, "wb");//训练结束 准备输出
	if (classes == 0) {
		// Save the word vectors
		fprintf(fo, "%lld %lld\n", vocab_size, layer1_size);//词汇量 隐层神经元数（词向量维数）
		fprintf(new_fo, "%lld %lld\n", weight_size, weight_layer_size);//词汇量 隐层神经元数（词向量维数）
		for (a = 0; a < vocab_size; a++) {
			fprintf(fo, "%s ", vocab[a].word);
			//printf("%s ", vocab[a].word);
			//for (b = 0; b < layer1_size; b++) printf("%lf ", syn0[a * layer1_size + b]);
			//printf("\n");
			if (binary) for (b = 0; b < layer1_size; b++) fwrite(&syn0[a * layer1_size + b], sizeof(real), 1, fo);
			else for (b = 0; b < layer1_size; b++) fprintf(fo, "%lf ", syn0[a * layer1_size + b]);
			fprintf(fo, "\n");
		}
		for (a = 0; a < weight_size; a++) {
			fprintf(new_fo, "%d ", a);
			//printf("%s ", vocab[a].word);
			//for (b = 0; b < layer1_size; b++) printf("%lf ", syn0[a * layer1_size + b]);
			//printf("\n");
			//if (binary) for (b = 0; b < layer1_size; b++) fwrite(&syn0[a * layer1_size + b], sizeof(real), 1, fo);
			//else 
			for (b = 0; b < weight_layer_size; b++) fprintf(new_fo, "%lf ", syn2[a * weight_layer_size + b]);
			fprintf(new_fo, "\n");
		}
	}
	else {//k-means聚类算法
		  // Run K-means on the word vectors
		int clcn = classes, iter = 10, closeid;
		int *centcn = (int *)malloc(classes * sizeof(int));
		int *cl = (int *)calloc(vocab_size, sizeof(int));
		real closev, x;
		real *cent = (real *)calloc(classes * layer1_size, sizeof(real));
		for (a = 0; a < vocab_size; a++) cl[a] = a % clcn;
		for (a = 0; a < iter; a++) {
			for (b = 0; b < clcn * layer1_size; b++) cent[b] = 0;
			for (b = 0; b < clcn; b++) centcn[b] = 1;
			for (c = 0; c < vocab_size; c++) {
				for (d = 0; d < layer1_size; d++) cent[layer1_size * cl[c] + d] += syn0[c * layer1_size + d];
				centcn[cl[c]]++;
			}
			for (b = 0; b < clcn; b++) {
				closev = 0;
				for (c = 0; c < layer1_size; c++) {
					cent[layer1_size * b + c] /= centcn[b];
					closev += cent[layer1_size * b + c] * cent[layer1_size * b + c];
				}
				closev = sqrt(closev);
				for (c = 0; c < layer1_size; c++) cent[layer1_size * b + c] /= closev;
			}
			for (c = 0; c < vocab_size; c++) {
				closev = -10;
				closeid = 0;
				for (d = 0; d < clcn; d++) {
					x = 0;
					for (b = 0; b < layer1_size; b++) x += cent[layer1_size * d + b] * syn0[c * layer1_size + b];
					if (x > closev) {
						closev = x;
						closeid = d;
					}
				}
				cl[c] = closeid;
			}
		}
		// Save the K-means classes
		//保存k-means类别
		for (a = 0; a < vocab_size; a++) fprintf(fo, "%s %d\n", vocab[a].word, cl[a]);
		free(centcn);
		free(cent);
		free(cl);
	}
	for (a = 0;a < MAX_YICUN;a++){
		fprintf(weight_fo, "%f\n", weight[a]);
	}
	fclose(fo);
	fclose(new_fo);
	fclose(weight_fo);
}

int ArgPos(char *str, int argc, char **argv) {
	int a;
	for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
		if (a == argc - 1) {
			printf("Argument missing for %s\n", str);
			exit(1);
		}
		return a;
	}
	return -1;
}

int main(int argc, char **argv) {
	int i;
	if (argc == 1) {
		printf("WORD VECTOR estimation toolkit v 0.1c\n\n");
		printf("Options:\n");
		printf("Parameters for training:\n");
		printf("\t-train <file>\n");
		printf("\t\tUse text data from <file> to train the model\n");
		printf("\t-output <file>\n");
		printf("\t\tUse <file> to save the resulting word vectors / word clusters\n");
		printf("\t-new-output <file>\n");
		printf("\t\tUse <file> to save the new resulting word vectors / word clusters\n");
		printf("\t-weight-output <file>\n");
		printf("\t\tUse <file> to save the resulting weigths(for cbow)\n");
		printf("\t-size <int>\n");
		printf("\t\tSet size of word vectors; default is 100\n");
		printf("\t-window <int>\n");
		printf("\t\tSet max skip length between words; default is 5\n");
		printf("\t-sample <float>\n");
		printf("\t\tSet threshold for occurrence of words. Those that appear with higher frequency in the training data\n");
		printf("\t-weight-sample <float>\n");
		printf("\t\tSet threshold for occurrence of wweights. Those that appear with higher frequency in the training data\n");
		printf("\t\twill be randomly down-sampled; default is 1e-3, useful range is (0, 1e-5)\n");
		printf("\t-hs <int>\n");
		printf("\t\tUse Hierarchical Softmax; default is 0 (not used)\n");
		printf("\t-negative <int>\n");
		printf("\t\tNumber of negative examples; default is 5, common values are 3 - 10 (0 = not used)\n");
		printf("\t-threads <int>\n");
		printf("\t\tUse <int> threads (default 12)\n");
		printf("\t-iter <int>\n");
		printf("\t\tRun more training iterations (default 5)\n");
		printf("\t-min-count <int>\n");
		printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
		printf("\t-alpha <float>\n");
		printf("\t\tSet the starting learning rate; default is 0.025 for skip-gram and 0.05 for CBOW\n");
		printf("\t-classes <int>\n");
		printf("\t\tOutput word classes rather than word vectors; default number of classes is 0 (vectors are written)\n");
		printf("\t-debug <int>\n");
		printf("\t\tSet the debug mode (default = 2 = more info during training)\n");
		printf("\t-binary <int>\n");
		printf("\t\tSave the resulting vectors in binary moded; default is 0 (off)\n");
		printf("\t-save-vocab <file>\n");
		printf("\t\tThe vocabulary will be saved to <file>\n");
		printf("\t-read-vocab <file>\n");
		printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
		printf("\t-read-weightcn <file>\n");
		printf("\t\tThe weight's cn will be read from <file>, not constructed from the training data\n");
		printf("\t-cbow <int>\n");
		printf("\t\tUse the continuous bag of words model; default is 1 (use 0 for skip-gram model)\n");
		printf("\t-new_operation <int>\n");
		printf("\t\tUse new_operation to train words model; default is 0 (use 1 for using)\n");
		printf("\nExamples:\n");
		printf("./word2vec -train data.txt -output vec.txt -size 200 -window 5 -sample 1e-4 -negative 5 -hs 0 -binary 0 -cbow 1 -iter 3\n\n");
		return 0;
	}
	output_file[0] = 0;
	new_output_file[0] = 0;
	weight_output_file[0] = 0;
	save_vocab_file[0] = 0;
	read_vocab_file[0] = 0;
	read_weightcn[0] = 0;
	if ((i = ArgPos((char *)"-size", argc, argv)) > 0) layer1_size = atoi(argv[i + 1]);//指定word vector的维度 对应隐层神经元个数 默认100
	if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);//训练指定文件
	if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);//指定词汇表存储文件
	if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);//指定词汇表加载文件
	if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);//调试等级 默认为2
	if ((i = ArgPos((char *)"-binary", argc, argv)) > 0) binary = atoi(argv[i + 1]);//指定是都将结果输出为二进制文件 默认0 不输出二进制
	if ((i = ArgPos((char *)"-cbow", argc, argv)) > 0) cbow = atoi(argv[i + 1]);//指定是否使用cbow框架
	//if (cbow) alpha = 0.05;
	if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);//制定出使学习速率 默认0.25
	if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);//指定输出文件 存储word vectors 或单词聚类结果
	if ((i = ArgPos((char *)"-new-output", argc, argv)) > 0) strcpy(new_output_file, argv[i + 1]);//指定输出文件 存储new word vectors 或单词聚类结果
	if ((i = ArgPos((char *)"-weight-output", argc, argv)) > 0) strcpy(weight_output_file, argv[i + 1]);//指定输出文件 存储weights结果
	if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);//指定窗口大小 在cbow中表示word vector最大叠加范围 在skip-gram中表示单词间的最大间隔 默认5
	if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);//指定亚采样拒绝概率参数
	if ((i = ArgPos((char *)"-weight-sample", argc, argv)) > 0) weight_sample = atof(argv[i + 1]);//指定亚采样拒绝概率参数
	if ((i = ArgPos((char *)"-hs", argc, argv)) > 0) hs = atoi(argv[i + 1]);//指定是否使用hs 默认1
	if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);//指定使用ns时负采样个数
	if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);//指定线程域
	if ((i = ArgPos((char *)"-iter", argc, argv)) > 0) iter = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-new_operation", argc, argv)) > 0) new_operation = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-read-weightcn", argc, argv)) > 0) strcpy(read_weightcn, argv[i + 1]);
	if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);//指定长尾词词频阈值
	if ((i = ArgPos((char *)"-classes", argc, argv)) > 0) classes = atoi(argv[i + 1]);//指定是否输出单词类 默认0 不输出
	vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
	vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
	expTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
	if (new_operation == 1) {
		printf("new_operation\n");
		//初始化所有依存关系的权值
		for (i = 0;i < MAX_YICUN;i++) {
			weight[i] = 0.8;
		}
	}
	for (i = 0; i <= EXP_TABLE_SIZE; i++) {
		expTable[i] = exp((i / (real)EXP_TABLE_SIZE * 2 - 1) * MAX_EXP); // Precompute the exp() table
		expTable[i] = expTable[i] / (expTable[i] + 1);                   // Precompute f(x) = x / (x + 1)
	}
	TrainModel();
	return 0;
}
