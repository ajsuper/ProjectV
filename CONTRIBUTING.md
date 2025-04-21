# Welcome!
Hello!! Thank you for visiting our contributions file! If you choose to contribute, then please read the rest of this file to gather the necessary information to help out the project! Please read over our [CODE_OF_CONDUCT.md](/CODE_OF_CONDUCT.md) before contributing, and if you haven't already, visit our [README.md](/README.md) to get an overview of the project! Also be sure to visit the copy of the [MIT License](/LICENSE.md).

### Changes to the API:

If you make any changes to the API that might interfere with existing programs, please ensure that these changes are reflected in the examples section [EXAMPLES](/docs/examples)

### Philosophy

#### This is the design philosophy we follow:

ProjectV aims to allow upmost flexibility to the user, allowing them to easily use and modify any part of the software they like. This means comments should be clear, concise, and provide meaningful explanations of the code. Code should be allowed to be used for as many use cases as possible, if a function performs two complex tasks, it becomes two seperate functions, allowing for it to be used in other parts of the code.

### Paradigm

#### This is the programming paradigm we loosely try to stick to:

Designed with functional programming in mind due to its flexibility and the fact that the most complex parts of the code are writen in GLSL which doesn't support complex objects. Object Oriented features are used though because they can greatly enhance the code quality and performance in certain use cases.

#### Details:
- Seperation of concerns. If you have a struct or class, it should only store data. To add functionality you must create a function that takes this struct or class as input.
- Mutable parameters are allowed for performance reasons.
- Structs should never have methods inside of them. No constructors, instead use factory functions.

### Reporting Issues

If you find a bug or have a feature request, please create an issue on our GitHub repository. When reporting an issue, please include:

- A clear and descriptive title.
- A detailed description of the problem or suggestion.
- Steps to reproduce the issue (if applicable).
- Any relevant logs or screenshots.

### Submitting Pull Requests

To contribute code, please follow these steps:

1. Fork the repository.
2. Make your changes to your fork.
4. Ensure your code follows the coding standards outlined below and the paradigm outlined above and is tested for correct functionality.
5. Push your changes to your forked repository.
6. Create a pull request to the main repository.
7. Positively collaborate and discuss with anyone who comments on your pull request.

### Coding Standards

To maintain consistency in the codebase, please adhere to the following coding standards:

- Use meaningful variable and function names, both in camelCase.
- Use snake_case for any sort of callbacks.
- Write clear and concise inline comments and save large comments for the .h files.
- Tabs = 4 spaces
- Use: `// Text here.` for inline comments.
- Use:
```cpp
/**
  * This is a comment for describing a function or struct. Much larger, these are the ones in the .h files.
  *
  * @param nameOfParameter is what I take in.
  * @return nameOfOutput is what I return.
  */
```
  to add comments above functions or structs to allow for code editors to supply function descriptions and information.
```cpp
void thisIsAFunctionDefinition(int oneSpaceAfterThisParenthesis->) {
    int codeStartsRightAway; // Unrelated blocks are followed by one blank line.

    float Foo; // These 3 are related
    oneSpaceAfterTheLastThing(std::string weDoSomethingSimilarSoWereTogether, Foo);
    toEnsureItIsClearWhichThingsAreWhat(std::string weDoSomethingSimilarSoWereTogether, Foo); // I needed a comment, but they're still closely related, thus the comment goes to the right to ensure they're together.

    // This does something else in the function.
    if(usingIfStatements) {
        similarFormattingAsFunction
        doAThing(); // All a related block of code
        return conditionA;
    } 

    for(forLoopsDoTheSame) {
        a += index;
    }
    return conditionB;
}
```

### Code Review Process

All pull requests will be reviewed by a project maintainer. During the review process, you may be asked to make changes to your code. Please be responsive to feedback and make the necessary updates.

### Adding of Data Structures

If you add any data structure to the project that are significant enough that they require more explanation, then please include an explanation in the [data_structures](/data_structures) folder.

### Modules

Please see:         
- [utils.md](/include/utils/utils.md)
- [core.md](/include/core/core.md)
- [graphics.md](/include/graphics/graphics.md)

To assess where your changes belong. Also please add any new modules to their respective .md file for documentation purpouses.

### Citations

If your changes include anything that should be cited, please include it in the [SOURCES.md](/SOURCES.md). Use this format
```
### What It's Used For
  - [Name of source](Link to source) - Accesed on day/month/year
```

Thank you for contributing to ProjectV! Your help is greatly appreciated.

### More

For more information on this project, visit our [README.md](/README.md)
